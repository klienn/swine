// supabase/functions/ingest-live-frame/index.ts
import { createClient } from "https://esm.sh/@supabase/supabase-js@2?dts";
import { verify } from "./_auth.ts";
import { composeOverlay, type ThermalPayload } from "./_overlay.ts";

Deno.serve(async (req) => {
  const t0 = Date.now();
  try {
    const auth = await verify(req);
    if (!auth.ok) return new Response(auth.msg, { status: auth.status });

    const supabase = auth.supabase as ReturnType<typeof createClient>;
    const form = await req.formData();

    const cam = form.get("cam") as File | null;
    const thermalFile = form.get("thermal") as File | null;
    const readingFile = form.get("reading") as File | null;

    if (!cam || !thermalFile) {
      return new Response(JSON.stringify({ error: "missing_cam_or_thermal" }), {
        status: 400,
        headers: { "Content-Type": "application/json" },
      });
    }

    const camBytes = new Uint8Array(await cam.arrayBuffer());
    const thermal = JSON.parse(await thermalFile.text()) as ThermalPayload;

    // optional reading (never fail the request if insert errors)
    let readingId: number | null = null;
    if (readingFile) {
      try {
        const r = JSON.parse(await readingFile.text());
        const { data, error } = await supabase
          .from("readings")
          .insert({
            device_id: auth.devId,
            temp_c: r.tempC,
            humidity_rh: r.humidity,
            pressure_hpa: r.pressure,
            gas_res_ohm: r.gasRes,
            iaq: r.iaq,
            t_min_c: r.tMin,
            t_max_c: r.tMax,
            t_avg_c: r.tAvg,
          })
          .select("id")
          .single();
        if (!error) readingId = data.id;
      } catch (e) {
        console.error("reading insert failed:", e);
      }
    }

    // overlay (guarded + skippable)
    let outBytes = camBytes;
    let contentType = "image/jpeg";
    let overlayWarning: string | null = null;

    const skipOverlay = (Deno.env.get("SKIP_OVERLAY") ?? "") === "1";
    if (!skipOverlay) {
      try {
        const W = Number((thermal as any)?.w);
        const H = Number((thermal as any)?.h);
        const len = Array.isArray((thermal as any)?.data) ? (thermal as any).data.length : -1;

        // quick sanity checks to avoid ImageScript throwing
        if (
          Number.isFinite(W) &&
          Number.isFinite(H) &&
          W >= 2 &&
          H >= 2 &&
          len >= W * H &&
          camBytes.byteLength > 0
        ) {
          const t1 = Date.now();
          const alpha = Number(Deno.env.get("OVERLAY_ALPHA") ?? "0.35");
          const composed = await composeOverlay(camBytes, thermal, alpha);
          const t2 = Date.now();

          outBytes = composed;
          if (outBytes.length >= 8 && outBytes[0] === 0x89 && outBytes[1] === 0x50) {
            contentType = "image/png";
          } // else JPEG

          console.log(`overlay ok: cam=${camBytes.length}B, W×H=${W}×${H}, took=${t2 - t1}ms`);
        } else {
          overlayWarning = `overlay_skipped_bad_meta w=${W} h=${H} len=${len}`;
          console.warn(overlayWarning);
        }
      } catch (e) {
        overlayWarning = `composeOverlay failed: ${String((e as Error)?.message ?? e)}`;
        console.error(overlayWarning);
        outBytes = camBytes;
        contentType = "image/jpeg";
      }
    } else {
      overlayWarning = "overlay_skipped_env";
    }

    // upload to frames-live/current.jpg|png
    const objectPath = `${auth.devId}/current.${contentType === "image/png" ? "png" : "jpg"}`;
    const { error: upErr } = await supabase.storage
      .from("frames-live")
      .upload(objectPath, outBytes, {
        contentType,
        upsert: true,
        cacheControl: "no-store",
      });

    if (upErr) {
      console.error("storage upload failed:", upErr);
      return new Response(
        JSON.stringify({ error: "upload_failed", details: upErr.message ?? upErr }),
        { status: 500, headers: { "Content-Type": "application/json" } },
      );
    }

    // touch device
    await supabase
      .from("devices")
      .update({ last_seen: new Date().toISOString() })
      .eq("id", auth.devId);

    const dt = Date.now() - t0;
    return new Response(
      JSON.stringify({
        ok: true,
        path: objectPath,
        bytes: outBytes.byteLength,
        readingId,
        warning: overlayWarning,
        took_ms: dt,
      }),
      { status: 200, headers: { "Content-Type": "application/json" } },
    );
  } catch (err) {
    console.error("ingest-live-frame fatal:", err);
    return new Response(JSON.stringify({ error: "server_error", details: String(err) }), {
      status: 500,
      headers: { "Content-Type": "application/json" },
    });
  }
});
