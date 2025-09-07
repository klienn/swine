// supabase/functions/live-current/index.ts
import { createClient } from "@supabase/supabase-js";

Deno.serve(async (req) => {
  const url = new URL(req.url);
  const device = url.searchParams.get("device");
  if (!device) return new Response("device required", { status: 400 });

  const supabase = createClient(
    Deno.env.get("SUPABASE_URL")!,
    Deno.env.get("SUPABASE_SERVICE_ROLE_KEY")!,
  );
  const { data, error } = await supabase.storage
    .from("frames-live")
    .download(`frames-live/${device}/current.jpg`);
  if (error) return new Response("no frame", { status: 404 });

  const buf = await data.arrayBuffer();
  return new Response(buf, {
    status: 200,
    headers: { "Content-Type": "image/jpeg", "Cache-Control": "no-store, max-age=0" },
  });
});
