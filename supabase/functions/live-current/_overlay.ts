// supabase/functions/<fn>/_overlay.ts
import * as IS from "https://deno.land/x/imagescript@1.3.0/mod.ts";

export type ThermalPayload = { w: number; h: number; data: number[] };

const rgba = (r: number, g: number, b: number, a = 255) => (a << 24) | (r << 16) | (g << 8) | b;

export async function composeOverlay(
  camJpeg: Uint8Array,
  thermal: ThermalPayload,
  alphaRaw = 0.35,
): Promise<Uint8Array> {
  // Decode camera frame first
  const base = await IS.Image.decode(camJpeg);

  // Coerce & validate dimensions
  const W = Math.trunc(Number((thermal as any)?.w));
  const H = Math.trunc(Number((thermal as any)?.h));
  const data = Array.isArray(thermal?.data) ? thermal.data : [];

  // If dims are missing/invalid or tiny, just return original
  if (!Number.isFinite(W) || !Number.isFinite(H) || W < 2 || H < 2) {
    return await base.encodeJPEG(80);
  }

  const N = W * H;
  if (data.length < N) {
    return await base.encodeJPEG(80);
  }

  // Min/max
  let tMin = Infinity,
    tMax = -Infinity;
  for (let i = 0; i < N; i++) {
    const v = Number(data[i]);
    if (Number.isFinite(v)) {
      if (v < tMin) tMin = v;
      if (v > tMax) tMax = v;
    }
  }
  if (!Number.isFinite(tMin) || !Number.isFinite(tMax) || tMax - tMin <= 1e-6) {
    return await base.encodeJPEG(80);
  }

  const norm = (v: number) => {
    const t = (v - tMin) / (tMax - tMin + 1e-6);
    return t < 0 ? 0 : t > 1 ? 1 : t;
  };
  const mapColor = (t: number) => {
    const r = Math.round(255 * Math.min(1, Math.max(0, 1.7 * t)));
    const g = Math.round(255 * Math.min(1, Math.max(0, t * t)));
    const b = Math.round(255 * Math.min(1, Math.max(0, (1 - t) * (1 - t))));
    return rgba(r, g, b, 255);
  };

  // Build source heatmap
  const heat = new IS.Image(W, H);
  heat.fill(0x00000000);
  let idx = 0;
  for (let y = 0; y < H; y++) {
    for (let x = 0; x < W; x++, idx++) {
      const v = Number(data[idx]);
      heat.setPixelAt(x, y, mapColor(norm(Number.isFinite(v) ? v : tMin)));
    }
  }

  // Scale to camera size. If bilinear throws on tiny sources, do NN manually.
  let scaled: IS.Image;
  try {
    scaled = heat.resize(base.width, base.height); // bilinear
  } catch {
    // Nearest-neighbor fallback (robust for W/H=1..)
    scaled = new IS.Image(base.width, base.height);
    const sx = base.width / W;
    const sy = base.height / H;
    for (let y = 0; y < base.height; y++) {
      const yy = Math.min(H - 1, Math.max(0, Math.floor(y / sy)));
      for (let x = 0; x < base.width; x++) {
        const xx = Math.min(W - 1, Math.max(0, Math.floor(x / sx)));
        const c = heat.getPixelAt(xx, yy);
        scaled.setPixelAt(x, y, c);
      }
    }
  }

  const alpha = Math.min(1, Math.max(0, Number.isFinite(alphaRaw) ? alphaRaw : 0.35));
  base.drawImage(scaled, 0, 0, alpha);

  try {
    return await base.encodeJPEG(80);
  } catch {
    return await base.encodePNG();
  }
}
