// supabase/functions/ingest-live-frame/index.ts
import { createLiveIngestHandler } from "./live_ingest.ts";

// Overlay composition now happens on the device; this handler simply persists
// the most recent frame + thermal JSON for the mobile client to fetch.
const handler = createLiveIngestHandler({
  requireThermal: true,
  persistCamFrame: true,
  persistThermal: true,
});

Deno.serve(handler);
