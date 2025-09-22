// supabase/functions/ingest-live-frame/index.ts
import { createClient } from "https://esm.sh/@supabase/supabase-js@2?dts";
import { verify } from "./_auth.ts";
// Overlay is now handled on the device. We only persist the raw thermal
// payload so that the frontend can render it.

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
    // keep the raw thermal payload so the mobile app can compose the overlay
    // client-side. We don't parse it here since no server-side overlay occurs.
    const thermalJson = await thermalFile.text();

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

    // No server-side overlay; simply persist the raw camera frame
    // (assumed JPEG) and the raw thermal JSON so the client can overlay.
    const contentType = "image/jpeg";
    const framePath = `${auth.devId}/current.jpg`;
    const { error: upErr } = await supabase.storage
      .from("frames-live")
      .upload(framePath, camBytes, {
        contentType,
        upsert: true,
        cacheControl: "no-store",
      });

    // Persist thermal JSON alongside the frame for client use
    const thermalPath = `${auth.devId}/current.json`;
    await supabase.storage.from("frames-live").upload(thermalPath, thermalJson, {
      contentType: "application/json",
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
    return new Response(JSON.stringify({ ok: true, readingId, elapsed_ms: dt }), {
      status: 200,
      headers: { "Content-Type": "application/json" },
    });
  } catch (err) {
    console.error("ingest-live-frame fatal:", err);
    return new Response(JSON.stringify({ error: "server_error", details: String(err) }), {
      status: 500,
      headers: { "Content-Type": "application/json" },
    });
  }
});
