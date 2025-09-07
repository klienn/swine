import { createClient } from "https://esm.sh/@supabase/supabase-js@2?dts";
import { verify } from "./_auth.ts"; // your HMAC verifier (SR key client)
import { composeOverlay, type ThermalPayload } from "./_overlay.ts";

Deno.serve(async (req) => {
  try {
    const auth = await verify(req);
    if (!auth.ok) return new Response(auth.msg, { status: auth.status });

    const supabase = auth.supabase as ReturnType<typeof createClient>;
    const form = await req.clone().formData();

    const cam = form.get("cam") as File | null;
    const thermalFile = form.get("thermal") as File | null;
    const readingFile = form.get("reading") as File | null;

    if (!cam) {
      return new Response(JSON.stringify({ error: "missing_cam" }), {
        status: 400,
        headers: { "Content-Type": "application/json" },
      });
    }

    // ---- read camera bytes
    const camBytes = new Uint8Array(await cam.arrayBuffer());

    // ---- optional: insert a reading row first (so we can link it)
    let readingId: number | null = null;
    try {
      if (readingFile) {
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
        if (error) throw error;
        readingId = data.id;
      }
    } catch (e) {
      console.error("snapshot: reading insert failed:", e);
    }

    // ---- overlay (graceful fallback to raw bytes)
    let outBytes: Uint8Array = camBytes;
    let contentType = "image/jpeg";
    let overlayWarning: string | null = null;

    if (thermalFile) {
      try {
        const thermal = JSON.parse(await thermalFile.text()) as ThermalPayload;
        const alpha = Number(Deno.env.get("OVERLAY_ALPHA") ?? "0.35");
        outBytes = await composeOverlay(camBytes, thermal, alpha);
        // PNG signature? (ImageScript may fall back)
        if (outBytes.length >= 8 && outBytes[0] === 0x89 && outBytes[1] === 0x50) {
          contentType = "image/png";
        }
      } catch (e) {
        overlayWarning = String(e?.message ?? e);
        console.error("snapshot: composeOverlay failed:", overlayWarning);
        outBytes = camBytes; // use raw frame
        contentType = "image/jpeg";
      }
    }

    // ---- upload into Storage bucket 'snapshots' (append-only)
    const stamp = new Date().toISOString().replace(/[:.]/g, "-"); // safe filename
    const ext = contentType === "image/png" ? "png" : "jpg";
    const objectPath = `${auth.devId}/${stamp}.${ext}`;

    const { error: upErr } = await supabase.storage.from("snapshots").upload(objectPath, outBytes, {
      contentType,
      upsert: false,
      cacheControl: "no-store",
    });

    if (upErr) {
      console.error("snapshot: storage upload failed:", upErr);
      return new Response(
        JSON.stringify({ error: "upload_failed", details: upErr.message ?? upErr }),
        { status: 500, headers: { "Content-Type": "application/json" } },
      );
    }

    // ---- insert into public.snapshots (matches YOUR schema)
    // columns: id, device_id, ts (default now), reading_id, alert_id, overlay_path
    let snapshotId: number | null = null;
    try {
      const payload: Record<string, unknown> = {
        device_id: auth.devId,
        overlay_path: objectPath, // << IMPORTANT: your column name
      };
      if (readingId != null) payload.reading_id = readingId;

      const { data, error } = await supabase
        .from("snapshots")
        .insert(payload)
        .select("id")
        .single();

      if (error) {
        console.error("snapshot: table insert failed:", error);
      } else {
        snapshotId = data.id;
      }
    } catch (e) {
      console.error("snapshot: table insert threw:", e);
    }

    // ---- touch devices.last_seen (best effort)
    await supabase
      .from("devices")
      .update({ last_seen: new Date().toISOString() })
      .eq("id", auth.devId);

    return new Response(
      JSON.stringify({
        ok: true,
        snapshotId,
        overlay_path: objectPath,
        contentType,
        readingId,
        warning: overlayWarning,
      }),
      { status: 200, headers: { "Content-Type": "application/json" } },
    );
  } catch (err) {
    console.error("ingest-snapshot fatal:", err);
    return new Response(JSON.stringify({ error: "server_error", details: String(err) }), {
      status: 500,
      headers: { "Content-Type": "application/json" },
    });
  }
});
