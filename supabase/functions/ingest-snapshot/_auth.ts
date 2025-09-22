// supabase/functions/_shared/_auth.ts
import { createClient } from "@supabase/supabase-js";

const hex = (buf: ArrayBuffer) =>
  Array.from(new Uint8Array(buf))
    .map((b) => b.toString(16).padStart(2, "0"))
    .join("");

export async function verify(req: Request) {
  const cloned = req.clone();

  const supabase = createClient(
    Deno.env.get("SUPABASE_URL")!,
    Deno.env.get("SUPABASE_SERVICE_ROLE_KEY")!,
  );

  const devId = req.headers.get("X-Device-Id") ?? "";
  const ts = req.headers.get("X-Timestamp") ?? "";
  const sig = req.headers.get("X-Signature") ?? "";
  if (!devId || !ts || !sig) return { ok: false, status: 401, msg: "Missing auth headers" };

  // ~120s clock skew tolerance
  const skew = Math.abs(Date.now() - Number(ts));
  if (!Number.isFinite(skew) || skew > 120_000) {
    return { ok: false, status: 401, msg: "Clock skew too large" };
  }

  const { data: dev, error } = await supabase
    .from("devices")
    .select("id, secret")
    .eq("id", devId)
    .single();
  if (error || !dev) return { ok: false, status: 401, msg: "Device not found" };

  const url = new URL(req.url);

  // ✅ hash the body from the CLONE
  const bodyBytes = new Uint8Array(await cloned.arrayBuffer());
  const bodyHashBuf = await crypto.subtle.digest("SHA-256", bodyBytes);
  const bodyHex = hex(bodyHashBuf);

  const base = `${req.method}\n${url.pathname}\n${bodyHex}\n${ts}`;
  const key = await crypto.subtle.importKey(
    "raw",
    new TextEncoder().encode(dev.secret),
    { name: "HMAC", hash: "SHA-256" },
    false,
    ["sign"],
  );
  const sigBuf = await crypto.subtle.sign("HMAC", key, new TextEncoder().encode(base));
  const expected = hex(sigBuf);

  if (expected !== sig) return { ok: false, status: 401, msg: "Bad signature" };

  // Optionally: size guard (reject > ~2.5MB bodies)
  // if (bodyBytes.byteLength > 2_500_000) return { ok:false, status:413, msg:"Payload too large" };

  return {
    ok: true,
    status: 200,
    msg: "OK",
    devId: dev.id,
    supabase,
    deviceSecret: dev.secret,
  };
}
