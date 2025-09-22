import type { SupabaseClient } from "@supabase/supabase-js";

type SupabaseChannel = ReturnType<SupabaseClient<any, any, any>["channel"]>;
type ChannelSendArgs = { type: string; [key: string]: unknown };

type PushOptions = {
  joinTimeoutMs?: number;
};

const DEFAULT_JOIN_TIMEOUT_MS = 5_000;

const joinChannel = (channel: SupabaseChannel, timeoutMs: number): Promise<void> =>
  new Promise((resolve, reject) => {
    let settled = false;
    let timer: ReturnType<typeof setTimeout> | undefined;

    const settle = (action: () => void) => {
      if (settled) return;
      settled = true;
      if (timer) clearTimeout(timer);
      action();
    };

    timer = setTimeout(() => {
      settle(() => {
        reject(
          new Error(
            `Realtime channel ${channel.topic} timed out while subscribing after ${timeoutMs}ms`,
          ),
        );
      });
    }, timeoutMs);

    channel.subscribe((status, err) => {
      if (status === "SUBSCRIBED") {
        settle(resolve);
        return;
      }
      if (status === "CHANNEL_ERROR") {
        settle(() => {
          reject(err ?? new Error(`Realtime channel ${channel.topic} returned CHANNEL_ERROR`));
        });
        return;
      }
      if (status === "TIMED_OUT") {
        settle(() => {
          reject(new Error(`Realtime channel ${channel.topic} subscription timed out`));
        });
        return;
      }
      if (status === "CLOSED") {
        settle(() => {
          reject(new Error(`Realtime channel ${channel.topic} closed before subscribing`));
        });
      }
    });
  });

export const pushRealtimeMessage = async (
  supabase: SupabaseClient<any, any, any>,
  topic: string,
  payload: ChannelSendArgs,
  { joinTimeoutMs = DEFAULT_JOIN_TIMEOUT_MS }: PushOptions = {},
): Promise<void> => {
  const channel = supabase.channel(topic);

  try {
    await joinChannel(channel, joinTimeoutMs);
    const result = await channel.send(payload as any);
    if (result !== "ok") {
      throw new Error(`Realtime push to ${topic} failed with status "${result}"`);
    }
  } finally {
    try {
      await channel.unsubscribe();
    } catch {
      // ignore cleanup failures
    }
    try {
      await supabase.removeChannel(channel);
    } catch {
      // ignore cleanup failures
    }
  }
};
