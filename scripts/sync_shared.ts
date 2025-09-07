// deno run -A scripts/sync_shared.ts
import { copy } from "https://deno.land/std@0.224.0/fs/copy.ts";

const ROOT = "supabase/functions";
const SHARED = `${ROOT}/_shared`;

for await (const entry of Deno.readDir(ROOT)) {
  if (!entry.isDirectory) continue;
  const name = entry.name;
  if (name === "_shared") continue;
  await copy(SHARED, `${ROOT}/${name}/`, { overwrite: true });
  console.log(`synced _shared -> ${name}`);
}
