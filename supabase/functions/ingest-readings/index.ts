// supabase/functions/ingest-readings/index.ts
import { createClient } from "@supabase/supabase-js";
import { verify } from "./_auth.ts";

Deno.serve(async (req) => {
  const auth = await verify(req);
  if (!auth.ok) return new Response(auth.msg, { status: auth.status });
  const supabase = createClient(
    Deno.env.get("SUPABASE_URL")!,
    Deno.env.get("SUPABASE_SERVICE_ROLE_KEY")!,
  );
  const b = await req.json();

  const { data, error } = await supabase
    .from("readings")
    .insert({
      device_id: auth.devId,
      temp_c: b.tempC,
      humidity_rh: b.humidity,
      pressure_hpa: b.pressure,
      gas_res_ohm: b.gasRes,
      iaq: b.iaq,
      t_min_c: b.tMin,
      t_max_c: b.tMax,
      t_avg_c: b.tAvg,
    })
    .select("id")
    .single();
  if (error) return new Response(error.message, { status: 500 });
  return new Response(JSON.stringify({ id: data.id }), {
    status: 200,
    headers: { "Content-Type": "application/json" },
  });
});
