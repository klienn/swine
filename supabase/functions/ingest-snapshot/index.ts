import { createClient } from "https://esm.sh/@supabase/supabase-js@2?dts";
import { verify } from "./_auth.ts"; // your HMAC verifier (SR key client)
// Overlaying is now done on the device. We persist the thermal payload for
// client-side composition.

Deno.serve(async (req) => {
  try {
    const auth = await verify(req);
    if (!auth.ok) return new Response(auth.msg, { status: auth.status });

    const supabase = auth.supabase as ReturnType<typeof createClient>;
    const form = await req.clone().formData();

    const cam = form.get("cam") as File | null;
    const thermalFile = form.get("thermal") as File | null;
    const readingFile = form.get("reading") as File | null;

    console.log(
      `ingest-snapshot: dev=${auth.devId} cam=${cam?.size ?? 0}B thermal=${
        thermalFile?.size ?? 0
      }B reading=${readingFile?.size ?? 0}B`,
    );
    console.log(
      `thermal: ${thermalFile ? await thermalFile.text() : "none"}, reading: ${
        readingFile ? await readingFile.text() : "none"
      }`,
    );

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

    // ---- no server-side overlay; save camera frame and thermal JSON
    const contentType = "image/jpeg";
    const stamp = new Date().toISOString().replace(/[:.]/g, "-"); // safe filename
    const objectPath = `${auth.devId}/${stamp}.jpg`;

    const { error: upErr } = await supabase.storage.from("snapshots").upload(objectPath, camBytes, {
      contentType,
      upsert: false,
      cacheControl: "no-store",
    });

    if (thermalFile) {
      const thermalJson = await thermalFile.text();
      await supabase.storage.from("snapshots").upload(`${auth.devId}/${stamp}.json`, thermalJson, {
        contentType: "application/json",
        upsert: false,
        cacheControl: "no-store",
      });
    }

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
