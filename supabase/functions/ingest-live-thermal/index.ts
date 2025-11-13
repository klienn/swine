// supabase/functions/ingest-live-thermal/index.ts
import { createLiveIngestHandler } from "./live_ingest.ts";

// Thermal-only uploads reuse the same validation/storage pipeline but skip
// frame uploads entirely. This keeps the "latest thermal" cache synced even
// when the JPEG stream is throttled by the firmware.
const handler = createLiveIngestHandler({
  requireThermal: true,
  persistCamFrame: false,
  persistThermal: true,
});

Deno.serve(handler);
