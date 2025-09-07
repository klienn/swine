// supabase/functions/retention-cleanup/index.ts
import { createClient } from "@supabase/supabase-js";

Deno.serve(async () => {
  const supabase = createClient(
    Deno.env.get("SUPABASE_URL")!,
    Deno.env.get("SUPABASE_SERVICE_ROLE_KEY")!,
  );

  // purge snapshots older than per-device config (default 7d)
  const { data: snaps } = await supabase
    .from("snapshots")
    .select("id, device_id, ts, overlay_path");

  const now = Date.now();
  const toDelete: { id: number; path: string }[] = [];

  // fetch per-device configs once
  const { data: devs } = await supabase.from("devices").select("id, config");
  const cfgMap = new Map(devs?.map((d) => [d.id, d.config?.snapshot_retention_days ?? 7]));

  for (const s of snaps ?? []) {
    const keepDays = cfgMap.get(s.device_id) ?? 7;
    if (new Date(s.ts).getTime() < now - keepDays * 24 * 3600 * 1000) {
      toDelete.push({ id: s.id, path: s.overlay_path });
    }
  }

  if (toDelete.length) {
    // remove files
    await supabase.storage.from("snapshots").remove(toDelete.map((t) => t.path));
    // remove rows
    await supabase
      .from("snapshots")
      .delete()
      .in(
        "id",
        toDelete.map((t) => t.id),
      );
  }

  // purge readings older than device config (default 7d)
  // (do in SQL for efficiency)
  await supabase.rpc("purge_old_readings");
  return new Response(JSON.stringify({ ok: true, deleted: toDelete.length }), {
    headers: { "Content-Type": "application/json" },
  });
});
