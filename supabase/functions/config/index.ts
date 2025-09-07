// supabase/functions/config/index.ts
import { createClient } from "@supabase/supabase-js";
import { verify } from "./_auth.ts";

Deno.serve(async (req) => {
  const auth = await verify(req);
  if (!auth.ok) return new Response(auth.msg, { status: auth.status });
  const supabase = createClient(
    Deno.env.get("SUPABASE_URL")!,
    Deno.env.get("SUPABASE_SERVICE_ROLE_KEY")!,
  );

  const { data, error } = await supabase
    .from("devices")
    .select("camera_url, camera_host, config")
    .eq("id", auth.devId)
    .single();
  if (error) return new Response(error.message, { status: 500 });

  return new Response(JSON.stringify(data), {
    status: 200,
    headers: { "Content-Type": "application/json" },
  });
});
