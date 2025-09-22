import { createClient } from "https://esm.sh/@supabase/supabase-js@2?dts";
import { verify } from "./_auth.ts"; // your HMAC verifier (SR key client)
import {
  createAlert,
  type AlertDraft,
  type AlertKind,
  type AlertSeverity,
} from "./create-alert.ts";

const numberOrNull = (value: unknown) =>
  typeof value === "number" && Number.isFinite(value) ? value : null;

const normalizeFlag = (flag: unknown) => {
  if (typeof flag !== "string") return null;
  const trimmed = flag.trim();
  if (!trimmed) return null;
  return trimmed
    .replace(/([a-z])([A-Z])/g, "$1-$2")
    .replace(/[^a-zA-Z0-9_-]/g, "-")
    .replace(/-+/g, "-")
    .toLowerCase();
};

const friendlyReason = (reason: string) =>
  reason
    .replace(/([a-z])([A-Z])/g, "$1 $2")
    .replace(/[_-]+/g, " ")
    .trim();

const mapFlagToAlertKind = (flag: string): AlertKind | null => {
  const canonical = flag.toLowerCase();
  if (
    canonical.includes("fever") ||
    canonical.includes("temp-fever") ||
    canonical.includes("temperature")
  ) {
    return "TEMP_FEVER";
  }
  if (
    canonical.includes("air-quality") ||
    canonical.includes("airquality") ||
    canonical.includes("iaq") ||
    canonical.includes("gas")
  ) {
    return "AIR_QUALITY";
  }
  if (
    canonical.includes("offline") ||
    canonical.includes("disconnected") ||
    canonical.includes("no-signal")
  ) {
    return "DEVICE_OFFLINE";
  }
  return null;
};

const alertMessageForKind = (
  kind: AlertKind,
  reading: Record<string, unknown>,
): { severity: AlertSeverity; message: string } => {
  if (kind === AlertKind.TEMP_FEVER) {
    const max =
      numberOrNull(reading["tMax"]) ??
      numberOrNull(reading["t_max"]) ??
      numberOrNull(reading["tMaxC"]) ??
      numberOrNull(reading["t_max_c"]);
    const threshold =
      numberOrNull(reading["feverThresholdC"]) ?? numberOrNull(reading["fever_threshold_c"]);
    const delta = numberOrNull(reading["feverDelta"]) ?? numberOrNull(reading["fever_delta"]);
    const parts: string[] = [];
    if (max != null) parts.push(`max ${max.toFixed(2)}°C`);
    if (threshold != null) parts.push(`threshold ${threshold.toFixed(1)}°C`);
    if (delta != null) parts.push(`Δ +${delta.toFixed(2)}°C`);
    const suffix = parts.length ? ` (${parts.join(", ")})` : "";
    return {
      severity: AlertSeverity.CRIT,
      message: `Fever detected${suffix}.`,
    };
  }

  if (kind === AlertKind.AIR_QUALITY) {
    const iaq = numberOrNull(reading["iaq"]);
    const gasRatio = numberOrNull(reading["gasRatio"]);
    const metrics: string[] = [];
    if (iaq != null) metrics.push(`IAQ ${iaq.toFixed(0)}`);
    if (gasRatio != null) metrics.push(`gas ratio ${gasRatio.toFixed(2)}`);
    const suffix = metrics.length ? ` (${metrics.join(", ")})` : "";
    return {
      severity: AlertSeverity.WARN,
      message: `Air quality threshold exceeded${suffix}.`,
    };
  }

  if (kind === AlertKind.DEVICE_OFFLINE) {
    const rawReason = reading["triggerReason"];
    const pretty =
      typeof rawReason === "string" && rawReason.trim().length > 0
        ? friendlyReason(rawReason)
        : "Device offline";
    const suffix = pretty && pretty.toLowerCase() !== "device offline" ? ` (${pretty})` : "";
    return {
      severity: AlertSeverity.WARN,
      message: `Device offline detected${suffix}.`,
    };
  }
  return {
    severity: AlertSeverity.WARN,
    message: `Alert triggered.`,
  };
};

const buildAlertsFromReading = (reading: Record<string, unknown>): AlertDraft[] => {
  const kinds = new Set<AlertKind>();
  const unknownFlags = new Set<string>();

  const considerFlag = (flag: unknown) => {
    const normalized = normalizeFlag(flag);
    if (!normalized) return;
    const kind = mapFlagToAlertKind(normalized);
    if (kind) {
      kinds.add(kind);
    } else {
      unknownFlags.add(normalized);
    }
  };

  const rawFlags = Array.isArray(reading["triggerFlags"])
    ? (reading["triggerFlags"] as unknown[])
    : [];
  for (const raw of rawFlags) {
    considerFlag(raw);
  }
  if (Boolean(reading["feverDetected"])) flags.add("fever");
  if (Boolean(reading["airQualityElevated"])) flags.add("air-quality");
  if (flags.size === 0) {
    considerFlag(reading["triggerReason"]);
  }

  if (unknownFlags.size > 0) {
    console.warn(
      `snapshot: ignoring unrecognized alert flags: ${Array.from(unknownFlags).join(", ")}`,
    );
  }

  const alertDrafts: AlertDraft[] = [];
  for (const kind of kinds) {
    const { severity, message } = alertMessageForKind(kind, reading);
    alertDrafts.push({ kind, severity, message });
  }
  return alertDrafts;
};

Deno.serve(async (req) => {
  try {
    const auth = await verify(req);
    if (!auth.ok) return new Response(auth.msg, { status: auth.status });

    const supabase = auth.supabase as ReturnType<typeof createClient>;
    const form = await req.clone().formData();

    const cam = form.get("cam") as File | null;
    const thermalFile = form.get("thermal") as File | null;
    const readingFile = form.get("reading") as File | null;

    const thermalText = thermalFile ? await thermalFile.text() : null;
    const readingText = readingFile ? await readingFile.text() : null;

    console.log(
      `ingest-snapshot: dev=${auth.devId} cam=${cam?.size ?? 0}B thermal=${
        thermalFile?.size ?? 0
      }B reading=${readingFile?.size ?? 0}B`,
    );
    console.log(`thermal: ${thermalText ?? "none"}, reading: ${readingText ?? "none"}`);

    let readingPayload: Record<string, unknown> | null = null;
    if (readingText) {
      try {
        readingPayload = JSON.parse(readingText) as Record<string, unknown>;
      } catch (err) {
        console.error("snapshot: failed to parse reading payload:", err);
      }
    }
    const alertsToCreate = readingPayload ? buildAlertsFromReading(readingPayload) : [];
    const createdAlertIds: number[] = [];

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
      if (readingPayload) {
        const { data, error } = await supabase
          .from("readings")
          .insert({
            device_id: auth.devId,
            temp_c: numberOrNull(readingPayload["tempC"]),
            humidity_rh: numberOrNull(readingPayload["humidity"]),
            pressure_hpa: numberOrNull(readingPayload["pressure"]),
            gas_res_ohm: numberOrNull(readingPayload["gasRes"]),
            iaq: numberOrNull(readingPayload["iaq"]),
            t_min_c: numberOrNull(readingPayload["tMin"]),
            t_max_c: numberOrNull(readingPayload["tMax"]),
            t_avg_c: numberOrNull(readingPayload["tAvg"]),
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

    if (thermalText != null) {
      await supabase.storage.from("snapshots").upload(`${auth.devId}/${stamp}.json`, thermalText, {
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

    if (alertsToCreate.length) {
      for (const alert of alertsToCreate) {
        try {
          const alertId = await createAlert(supabase, auth.devId, alert, readingId);
          createdAlertIds.push(alertId);
        } catch (err) {
          console.error("snapshot: alert creation failed:", err);
        }
      }
      if (snapshotId != null && createdAlertIds.length > 0) {
        try {
          await supabase
            .from("snapshots")
            .update({ alert_id: createdAlertIds[0] })
            .eq("id", snapshotId);
        } catch (err) {
          console.error("snapshot: failed to link alert to snapshot:", err);
        }
      }
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
        alertIds: createdAlertIds,
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
