# Thermal Overlay Client Guide

The edge functions `ingest-live-frame` and `ingest-snapshot` no longer
compose thermal overlays on the server. Instead, each request uploads two
objects into Supabase storage so that the React Native client can render the
overlay locally:

```
frames-live/<device_id>/current.jpg   – raw camera frame
frames-live/<device_id>/current.json  – thermal payload

snapshots/<device_id>/<stamp>.jpg     – raw snapshot frame
snapshots/<device_id>/<stamp>.json    – thermal payload for the snapshot
```

## Thermal Payload Shape

The JSON file is a direct forward of the device payload:

```json
{ "w": <number>, "h": <number>, "data": [<float>, ...] }
```

`w` and `h` describe the thermal sensor resolution and `data` contains
row‑major temperature values. No normalisation is applied on the backend.

## Client‑Side Overlay Example (React Native)

```ts
// fetch URLs
const { data: frameUrl } = await supabase.storage
  .from('frames-live')
  .createSignedUrl(`${deviceId}/current.jpg`, 10);
const { data: thermalUrl } = await supabase.storage
  .from('frames-live')
  .createSignedUrl(`${deviceId}/current.json`, 10);

// download thermal payload
const thermalResp = await fetch(thermalUrl.signedUrl);
const thermal = await thermalResp.json();

// build heatmap bitmap using the same algorithm as `_overlay.ts`
// (simplified example)
function normalise(v: number, min: number, max: number) {
  const t = (v - min) / (max - min + 1e-6);
  return Math.min(1, Math.max(0, t));
}

function mapColour(t: number) {
  const r = Math.round(255 * Math.min(1, Math.max(0, 1.7 * t)));
  const g = Math.round(255 * Math.min(1, Math.max(0, t * t)));
  const b = Math.round(255 * Math.min(1, Math.max(0, (1 - t) * (1 - t))));
  return { r, g, b };
}

// convert the thermal grid into an RGBA array suitable for a Canvas/GL overlay
function thermalToRgba({ w, h, data }: any) {
  let tMin = Infinity, tMax = -Infinity;
  for (const v of data) {
    if (Number.isFinite(v)) {
      if (v < tMin) tMin = v;
      if (v > tMax) tMax = v;
    }
  }
  const rgba = new Uint8ClampedArray(w * h * 4);
  let i = 0;
  for (const v of data) {
    const { r, g, b } = mapColour(normalise(v, tMin, tMax));
    rgba[i++] = r; rgba[i++] = g; rgba[i++] = b; rgba[i++] = 255;
  }
  return { w, h, rgba };
}

// render using any canvas/GL library. For example with expo-gl:
// draw the camera frame then overlay the heatmap texture with desired alpha.
```

### Snapshots

Snapshots are stored with timestamped names. To fetch the associated thermal
payload, replace the `.jpg` extension with `.json` on the `overlay_path` stored
in the database.

## Environment Notes

* The backend never touches the thermal data except to store it.
* Files are uploaded with `Cache-Control: no-store`; clients should append a
  cache‑busting query param when polling.
* All overlay rendering must now be implemented in the React Native app.

