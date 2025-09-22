import type { SupabaseClient } from "@supabase/supabase-js";
import { pushRealtimeMessage } from "./realtime.ts";

export type AlertKind = "AIR_QUALITY" | "TEMP_FEVER" | "DEVICE_OFFLINE";
export type AlertSeverity = "INFO" | "WARN" | "CRIT";

export type AlertDraft = {
  kind: AlertKind;
  severity: AlertSeverity;
  message: string;
};

export const createAlert = async (
  supabase: SupabaseClient<any, any, any>,
  deviceId: string,
  alert: AlertDraft,
  readingId: number | null,
): Promise<number> => {
  const { data, error } = await supabase
    .from("alerts")
    .insert({
      device_id: deviceId,
      kind: alert.kind,
      severity: alert.severity,
      message: alert.message,
      reading_id: readingId ?? null,
    })
    .select("id")
    .single();

  if (error) throw error;

  const alertId = data?.id;
  if (typeof alertId !== "number") {
    throw new Error("alerts-create returned an unexpected response");
  }

  await pushRealtimeMessage(supabase, `realtime:device:${auth.devId}`, {
    type: "alert",
    payload: { id: data.id },
  });

  return alertId;
};
