// supabase/functions/_shared/live_ingest.ts
import { createClient } from "https://esm.sh/@supabase/supabase-js@2?dts";
import { verify } from "./_auth.ts";

type SupabaseClient = ReturnType<typeof createClient>;

type VerifySuccess = {
  ok: true;
  status: number;
  msg: string;
  devId: string;
  supabase: SupabaseClient;
};

type VerifyResult = VerifySuccess | { ok: false; status: number; msg: string };

export type LiveIngestOptions = {
  requireThermal?: boolean;
  persistCamFrame?: boolean;
  persistThermal?: boolean;
  camBucket?: string;
  thermalBucket?: string;
  camPath?: (deviceId: string) => string;
  thermalPath?: (deviceId: string) => string;
};

type LiveIngestDeps = {
  verifyFn?: (req: Request) => Promise<VerifyResult>;
  now?: () => number;
  logger?: Pick<typeof console, "error" | "log">;
};

const jsonResponse = (status: number, payload: Record<string, unknown>) =>
  new Response(JSON.stringify(payload), {
    status,
    headers: { "Content-Type": "application/json" },
  });

const asFile = (entry: FormDataEntryValue | null): File | null =>
  entry instanceof File ? entry : null;

const readText = async (entry: FormDataEntryValue | null): Promise<string | null> => {
  if (entry == null) return null;
  if (typeof entry === "string") return entry;
  return await entry.text();
};

const toReadingRow = (
  devId: string,
  payload: Record<string, unknown> | null,
): Record<string, unknown> | null => {
  if (!payload) return null;
  return {
    device_id: devId,
    temp_c: payload["tempC"],
    humidity_rh: payload["humidity"],
    pressure_hpa: payload["pressure"],
    gas_res_ohm: payload["gasRes"],
    iaq: payload["iaq"],
    t_min_c: payload["tMin"],
    t_max_c: payload["tMax"],
    t_avg_c: payload["tAvg"],
  };
};

const insertReading = async (
  supabase: SupabaseClient,
  devId: string,
  reading: Record<string, unknown> | null,
  logger: Pick<typeof console, "error" | "log">,
): Promise<number | null> => {
  const row = toReadingRow(devId, reading);
  if (!row) return null;
  try {
    const { data, error } = await supabase.from("readings").insert(row).select("id").single();
    if (error) throw error;
    return data?.id ?? null;
  } catch (err) {
    logger.error?.("live-ingest: reading insert failed", err);
    return null;
  }
};

const uploadJson = async (
  supabase: SupabaseClient,
  bucket: string,
  path: string,
  body: string,
): Promise<{ error: unknown | null }> => {
  const { error } = await supabase.storage.from(bucket).upload(path, body, {
    contentType: "application/json",
    upsert: true,
    cacheControl: "no-store",
  });
  return { error };
};

const uploadFrame = async (
  supabase: SupabaseClient,
  bucket: string,
  path: string,
  file: File,
): Promise<{ error: unknown | null }> => {
  const bytes = new Uint8Array(await file.arrayBuffer());
  const { error } = await supabase.storage.from(bucket).upload(path, bytes, {
    contentType: file.type || "image/jpeg",
    upsert: true,
    cacheControl: "no-store",
  });
  return { error };
};

export const createLiveIngestHandler = (
  options: LiveIngestOptions,
  deps: LiveIngestDeps = {},
) => {
  const {
    requireThermal = true,
    persistCamFrame = true,
    persistThermal = true,
    camBucket = "frames-live",
    thermalBucket = "frames-live",
    camPath = (deviceId: string) => `${deviceId}/current.jpg`,
    thermalPath = (deviceId: string) => `${deviceId}/current.json`,
  } = options;
  const verifyFn = deps.verifyFn ?? verify;
  const now = deps.now ?? (() => Date.now());
  const logger = deps.logger ?? console;

  return async (req: Request): Promise<Response> => {
    const started = now();
    try {
      const auth = await verifyFn(req);
      if (!auth.ok) return new Response(auth.msg, { status: auth.status });
      const supabase = auth.supabase as SupabaseClient;

      const form = await req.formData();
      const camFile = asFile(form.get("cam"));
      const thermalEntry = form.get("thermal");
      const readingEntry = form.get("reading");

      if (requireThermal && !thermalEntry) {
        return jsonResponse(400, { error: "missing_thermal" });
      }

      const thermalJson = await readText(thermalEntry);

      let readingPayload: Record<string, unknown> | null = null;
      const readingText = await readText(readingEntry);
      if (readingText) {
        try {
          const parsed = JSON.parse(readingText);
          if (parsed && typeof parsed === "object") {
            readingPayload = parsed as Record<string, unknown>;
          }
        } catch (err) {
          logger.error?.("live-ingest: reading JSON parse failed", err);
        }
      }

      const readingId = await insertReading(supabase, auth.devId, readingPayload, logger);

      if (thermalJson && persistThermal) {
        const { error } = await uploadJson(supabase, thermalBucket, thermalPath(auth.devId), thermalJson);
        if (error) {
          logger.error?.("live-ingest: thermal upload failed", error);
          return jsonResponse(500, {
            error: "thermal_upload_failed",
            details: typeof error === "object" && error ? (error as any).message ?? String(error) : String(error),
          });
        }
      }

      if (camFile && persistCamFrame) {
        const { error } = await uploadFrame(supabase, camBucket, camPath(auth.devId), camFile);
        if (error) {
          logger.error?.("live-ingest: frame upload failed", error);
          return jsonResponse(500, {
            error: "frame_upload_failed",
            details: typeof error === "object" && error ? (error as any).message ?? String(error) : String(error),
          });
        }
      }

      await supabase
        .from("devices")
        .update({ last_seen: new Date().toISOString() })
        .eq("id", auth.devId);

      const elapsed = now() - started;
      return jsonResponse(200, { ok: true, readingId, elapsed_ms: elapsed });
    } catch (err) {
      logger.error?.("live-ingest: fatal", err);
      return jsonResponse(500, { error: "server_error", details: String(err) });
    }
  };
};

