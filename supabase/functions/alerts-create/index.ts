// supabase/functions/alerts-create/index.ts
import { createClient } from "@supabase/supabase-js";
import { verify } from "./_auth.ts";

Deno.serve(async (req) => {
  const auth = await verify(req);
  if (!auth.ok) return new Response(auth.msg, { status: auth.status });
  const supabase = createClient(
    Deno.env.get("SUPABASE_URL")!,
    Deno.env.get("SUPABASE_SERVICE_ROLE_KEY")!,
  );

  const body = await req.json();
  const { data, error } = await supabase
    .from("alerts")
    .insert({
      device_id: auth.devId,
      kind: body.kind,
      severity: body.severity,
      message: body.message,
      reading_id: body.reading_id ?? null,
    })
    .select("id")
    .single();
  if (error) return new Response(error.message, { status: 500 });

  await supabase
    .channel(`realtime:device:${auth.devId}`)
    .send({ type: "alert", payload: { id: data.id } });
  return new Response(JSON.stringify({ id: data.id }), {
    status: 200,
    headers: { "Content-Type": "application/json" },
  });
});
