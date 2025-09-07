// supabase/functions/camera-heartbeat/index.ts
import { createClient } from "@supabase/supabase-js";
import { verify } from "./_auth.ts";

Deno.serve(async (req) => {
  const auth = await verify(req);
  if (!auth.ok) return new Response(auth.msg, { status: auth.status });
  const supabase = createClient(
    Deno.env.get("SUPABASE_URL")!,
    Deno.env.get("SUPABASE_SERVICE_ROLE_KEY")!,
  );

  const body = await req.json(); // { ip: "192.168.1.23" }
  const url = `http://${body.ip}/capture?res=VGA`;

  const { error } = await supabase
    .from("devices")
    .update({ camera_url: url, last_seen: new Date().toISOString() })
    .eq("id", auth.devId);
  if (error) return new Response(error.message, { status: 500 });

  return new Response(JSON.stringify({ ok: true }), {
    status: 200,
    headers: { "Content-Type": "application/json" },
  });
});
