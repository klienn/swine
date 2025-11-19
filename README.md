# SwineTrack – Right on Time, Healthy Swine

SwineTrack is an end-to-end sensing platform for pig pens. A primary ESP32 collects BME680 air-quality data, MLX90640 thermal grids, and pulls RGB frames from an ESP32-CAM. The firmware pushes signed telemetry to Supabase Functions/Storage, exposes a local real-time thermal stream, and drives the same datasets that power the mobile application APIs documented below.

---

## Table of Contents
- [Overview](#overview)
- [Repository Layout](#repository-layout)
- [Hardware Reference](#hardware-reference)
- [Firmware Setup](#firmware-setup)
  - [swine-esp32 (sensor + uploader)](#swine-esp32-sensor--uploader)
  - [swine-esp32-cam (optical module)](#swine-esp32-cam-optical-module)
- [Local Thermal Endpoints](#local-thermal-endpoints)
- [Cloud Pipeline & APIs](#cloud-pipeline--apis)
  - [Edge Upload Endpoints](#edge-upload-endpoints)
    - [GET /config](#get-config)
    - [POST /ingest-live-thermal](#post-ingest-live-thermal)
    - [POST /ingest-live-frame](#post-ingest-live-frame)
    - [POST /ingest-snapshot](#post-ingest-snapshot)
  - [Client/App Data Access](#clientapp-data-access)
    - [1. Live](#1-live)
    - [2. Snapshots (Diary)](#2-snapshots-diary)
    - [3. Readings (History)](#3-readings-history)
    - [4. Alerts](#4-alerts)
- [License](#license)

---

## Overview

The updated deployment uses **two ESP32-class boards on the same Wi-Fi**:

- `swine-esp32` (DevKitC/WROVER class) owns the sensors, hosts a local SSE thermal stream, and uploads multi-part payloads to Supabase Functions using an HMAC signature (`X-Device-Id`, `X-Timestamp`, `X-Signature`).
- `swine-esp32-cam` (AI Thinker-style ESP32-CAM) stays close to the animals and exposes `http://<cam>/capture?...` for RGB frames. The main board polls this endpoint so the heavier neural/video work can happen in the cloud.

On the cloud side the data lands in Storage buckets (`frames-live`, `snapshots`) and Postgres tables (`readings`, `alerts`). The React Native sample code from the old README still applies, so it lives in the **Client/App Data Access** section.

---

## Repository Layout

- `swine-esp32/` – main firmware (`swine-esp32.ino`) plus helpers:
  - `swinetrack_sensors.h` – BME680 + MLX90640 setup, VOC heuristics, JSON builders.
  - `async_http_uploader.h` – background TLS uploader with retry/backpressure.
  - `swinetrack_http.h` – Wi-Fi/time helpers, camera fetch, remote config, TLS CA (`swt_certs.h`).
  - `swinetrack_local_server.h` – lightweight HTTP/SSE server for thermal previews.
  - `esp32_bb.png` / `esp32.fzz` – wiring reference for the sensor shield.
- `swine-esp32-cam/` – ESP32-CAM sketch based on Espressif’s WebServer example (`board_config.h`, `app_httpd.cpp`).
- `README.md` – this document.

---

## Hardware Reference

Minimum bill of materials:

- ESP32 DevKitC / WROVER module with PSRAM (main board).
- ESP32-CAM (AI-Thinker profile by default).
- MLX90640 thermal array (I²C @ 0x33).
- BME680 environmental sensor (I²C @ 0x76/0x77).
- Shared 5 V supply for camera and 3.3 V for the sensors.

`swine-esp32/esp32_bb.png` and `swine-esp32/esp32.fzz` show the exact wiring. Core signals:

| Peripheral | ESP32 Pin | Notes |
|------------|-----------|-------|
| I²C SDA    | GPIO21    | Shared between BME680 + MLX90640 |
| I²C SCL    | GPIO22    | 100 kHz at boot, bumped to 400 kHz after init |
| Camera     | Wi-Fi     | `CAMERA_URL` must resolve from the main ESP32 |

Keep the two boards on the same SSID. The `swine-esp32` firmware will reboot itself if Wi-Fi or SNTP cannot be re-established.

---

## Firmware Setup

### swine-esp32 (sensor + uploader)

1. **Install libraries**: ESP32 board support ≥ 3.x, `Adafruit BME680`, `Adafruit MLX90640`, `ArduinoJson`, plus the default ESP32 Wi-Fi stack (already bundled).
2. **Edit configuration constants** in `swine-esp32/swine-esp32.ino`:
   - `WIFI_SSID` / `WIFI_PASS` – WPA2 credentials.
   - `FN_BASE` – Supabase Functions base URL (`https://<ref>.functions.supabase.co`).
   - `DEVICE_ID` / `DEVICE_SECRET` – provisioned per device; the secret signs every HTTPS request.
   - `CAMERA_URL` – default `http://cam-pen1.local/capture?res=VGA` but overridden remotely via `/config`.
   - `LIVE_FRAME_INTERVAL_MS`, `READING_INTERVAL_MS`, `REALTIME_THERMAL_INTERVAL_MS`, `FEVER_C`, etc. (all tunable via `/config`).
3. **Verify certificates** inside `swine-esp32/swt_certs.h`. Replace the pin if Supabase rotates their CA chain.
4. **Build & flash** with an “ESP32 Dev Module” (or WROVER) profile and a partition scheme that offers ≥3 MB for the app plus PSRAM.

Behavior highlights:

- Wi-Fi guard + SNTP sync keep the device online; if cloud sync fails it still publishes to the local SSE server.
- The thermal server (port `8787`) publishes events every `REALTIME_THERMAL_INTERVAL_MS` (~50 ms) and survives even when Supabase is unreachable.
- Remote configuration: once time sync succeeds, the device hits `GET /config` for `camera_url`, `live_frame_interval_s`, `reading_interval_s`, `overlay_alpha`, and `fever_c`. Failures just fall back to the compiled defaults.
- `AsyncUploader` batches multi-part POSTs on core 1. It drops stale live frames (>10 s) and prioritizes alerts via `enqueuePriority`.
- Snapshots trigger when `MLX90640` reports `tMax > FEVER_C` or the BME680 gas resistance drops more than `kAirQualityRatioThreshold` (1.5×) from baseline. A five-minute cooldown prevents alert spam.

### swine-esp32-cam (optical module)

- Uses Espressif’s camera HTTP server with the AI-Thinker pinout (`board_config.h`). Enable a different board by uncommenting the relevant macro.
- Configure Wi-Fi credentials in `swine-esp32-cam/swine-esp32-cam.ino` (`ssid`, `password`). Use the same network as the main board to allow zeroconf hostnames like `cam-pen1.local`.
- After flashing, the camera exposes:
  - `http://<cam-ip>/capture` (what the main board hits, optionally with `?res=VGA` / `SVGA`),
  - `http://<cam-ip>/stream` (for debugging from a browser),
  - the default control UI at the root path.
- Keep PSRAM enabled for dual frame buffers; otherwise the firmware auto-drops to SVGA.

---

## Local Thermal Endpoints

`swine-esp32` exposes a helper server on `http://<device-ip>:8787` (hostname defaults to `esp32main.local`). Endpoints:

- `/status` – `{"status":"ok","path":"/thermal/latest","stream":"/thermal-stream"}` for health checks.
- `/thermal/latest` – last JSON payload that was published, shaped as:
  ```json
  {
    "capturedAt": 1714078123456,
    "tMin": 28.42,
    "tMax": 39.73,
    "tAvg": 32.11,
    "thermal": {
      "w": 32,
      "h": 24,
      "data": [... 768 floats ...],
      "tMin": 28.42,
      "tMax": 39.73,
      "tAvg": 32.11
    }
  }
  ```
- `/thermal-stream` – Server-Sent Events (`event: thermal`) pushing the same payload once the MLX90640 reader publishes it.

Example:
```
curl http://esp32main.local:8787/thermal/latest | jq .
curl http://esp32main.local:8787/thermal-stream
```

These endpoints run regardless of cloud connectivity so you can debug aiming/focus in the field.

---

## Cloud Pipeline & APIs

### Edge Upload Endpoints

All HTTPS calls go to `${FN_BASE}` over TLS 1.2 with CA pinning (`SUPABASE_ROOT_CA`). Every request includes:

- `X-Device-Id` – matches `DEVICE_ID`.
- `X-Timestamp` – `nowMs()` (Unix epoch in milliseconds).
- `X-Signature` – `HMAC_SHA256(deviceSecret, "<METHOD>\n<path>\n<bodyHash>\n<timestamp>")`.

`bodyHash` is `sha256Hex` of the raw multipart body. Multipart parts (when present) follow these names:

| Part name | Content type      | Description |
|-----------|-------------------|-------------|
| `cam`     | `image/jpeg`      | Binary frame pulled from the ESP32-CAM. |
| `thermal` | `application/json`| Output of `makeThermalJson` (32×24 grid, min/max/avg). |
| `reading` | `application/json`| BME680 scalars + thermal summary (`makeReadingJson` / alert context). |

#### GET /config

- **Purpose** – allow remote tuning without reflashing.
- **Response** – example:
  ```json
  {
    "camera_url": "http://cam-pen1.local/capture?res=VGA",
    "config": {
      "live_frame_interval_s": 1,
      "reading_interval_s": 180,
      "overlay_alpha": 0.35,
      "fever_c": 39.5
    }
  }
  ```
- The firmware retries every 120 s until `gConfigFetched` becomes true.

#### POST /ingest-live-thermal

- Pushed every `LIVE_FRAME_INTERVAL_MS` as long as the cloud is reachable.
- Contains **thermal** and **reading** JSON parts, no camera JPEG (saves bandwidth when the RGB fetch fails).
- Server should respond within 60 s so the uploader’s queue does not back up.

#### POST /ingest-live-frame

- Sent right after the thermal packet if pulling the camera JPEG succeeded.
- Includes all three parts (`cam`, `thermal`, `reading`). Frames are skipped when the queue has ≤1 free slot to keep the stream fresh.
- Ideal place to update the `frames-live/<device_id>/current.jpg` object used by the app.

#### POST /ingest-snapshot

- Triggered on gas anomalies or fever spikes (`FEVER_C`). Uses `enqueuePriority` so the uploader clears any stale work first.
- Carries the same multipart payload as live frames but should create a `snapshots` record + overlay asset + `alerts` entry.
- Device enforces a 5-minute cooldown (`SNAPSHOT_COOLDOWN_MS`) before sending another alert.

### Client/App Data Access

Mobile/web clients read data straight from Supabase buckets + Postgres. The helpers below are unchanged from the earlier README and still apply to the refreshed pipeline.

#### 1. Live
Each device continuously overwrites:
```
frames-live/<device_id>/current.jpg (or .png)
```

##### API + Hook
```ts
// src/features/live/api.ts
import { supabase } from '@/lib/supabase';

export async function getLiveFrameUrl(deviceId: string, ttlSec = 10) {
  const jpg = await supabase.storage.from('frames-live').createSignedUrl(`${deviceId}/current.jpg`, ttlSec);
  if (jpg.data?.signedUrl) return `${jpg.data.signedUrl}&cb=${Date.now()}`;
  const png = await supabase.storage.from('frames-live').createSignedUrl(`${deviceId}/current.png`, ttlSec);
  if (png.data?.signedUrl) return `${png.data.signedUrl}&cb=${Date.now()}`;
  throw new Error('no_live_frame');
}
```

```ts
// src/features/live/useLiveFrame.ts
import { useEffect, useState } from 'react';
import { getLiveFrameUrl } from './api';

export function useLiveFrame(deviceId: string, intervalMs = 1000) {
  const [url, setUrl] = useState<string|null>(null);
  const [err, setErr] = useState<string|null>(null);

  useEffect(() => {
    let alive = true, timer: any;
    const tick = async () => {
      try { setUrl(await getLiveFrameUrl(deviceId, 10)); setErr(null); }
      catch (e:any) { setErr(String(e?.message ?? e)); }
      finally { if (alive) timer = setTimeout(tick, intervalMs); }
    };
    tick();
    return () => { alive = false; clearTimeout(timer); };
  }, [deviceId, intervalMs]);

  return { url, err };
}
```

##### Screen Example
```tsx
// src/screens/LiveScreen.tsx
import React from 'react';
import { View, Image, ActivityIndicator, Text } from 'react-native';
import { useDevice } from '@/context/DeviceContext';
import { useLiveFrame } from '@/features/live/useLiveFrame';

export default function LiveScreen() {
  const { deviceId } = useDevice();
  const { url, err } = useLiveFrame(deviceId!, 1000); // 1 fps polling

  if (!deviceId) return <Text>Select a device</Text>;
  if (!url) return <ActivityIndicator />;

  return (
    <View style={{ padding: 12 }}>
      <Image source={{ uri: url }} style={{ width: '100%', aspectRatio: 4/3 }} />
      {err ? <Text style={{ color: 'orange', marginTop: 8 }}>warning: {err}</Text> : null}
    </View>
  );
}
```

#### 2. Snapshots (Diary)
Database table:
```
snapshots(device_id, ts, overlay_path, reading_id, alert_id)
```

##### API
```ts
// src/features/snapshots/api.ts
import { supabase } from '@/lib/supabase';

export type SnapshotRow = {
  id: number; device_id: string; ts: string;
  overlay_path: string; reading_id: number|null;
  reading?: {
    ts: string; temp_c: number|null; humidity_rh: number|null;
    pressure_hpa: number|null; gas_res_ohm: number|null; iaq: number|null;
    t_min_c: number|null; t_max_c: number|null; t_avg_c: number|null;
  } | null;
};

export async function listSnapshots(deviceId: string, page=0, pageSize=20) {
  const { data, error } = await supabase
    .from('snapshots')
    .select(`
      id, device_id, ts, overlay_path, reading_id,
      reading:readings ( ts, temp_c, humidity_rh, pressure_hpa, gas_res_ohm, iaq, t_min_c, t_max_c, t_avg_c )
    `)
    .eq('device_id', deviceId)
    .order('ts', { ascending: false })
    .range(page*pageSize, page*pageSize + pageSize - 1);

  if (error) throw error;

  return Promise.all((data ?? []).map(async (row:any) => {
    const { data: s } = await supabase.storage
      .from('snapshots')
      .createSignedUrl(row.overlay_path, 60*30);
    return { ...row, imageUrl: s?.signedUrl ?? null };
  }));
}
```

#### 3. Readings (History)
Query last 7 days by default.

##### API
```ts
// src/features/readings/api.ts
import { supabase } from '@/lib/supabase';

export type ReadingRow = {
  id:number; device_id:string; ts:string;
  temp_c:number|null; humidity_rh:number|null; pressure_hpa:number|null;
  gas_res_ohm:number|null; iaq:number|null; t_min_c:number|null; t_max_c:number|null; t_avg_c:number|null;
};

export async function fetchReadings(deviceId: string, fromISO: string, toISO: string, limit=1000) {
  const { data, error } = await supabase
    .from('readings')
    .select('id, device_id, ts, temp_c, humidity_rh, pressure_hpa, gas_res_ohm, iaq, t_min_c, t_max_c, t_avg_c')
    .eq('device_id', deviceId)
    .gte('ts', fromISO)
    .lte('ts', toISO)
    .order('ts', { ascending: false })
    .limit(limit);
  if (error) throw error;
  return data as ReadingRow[];
}
```

#### 4. Alerts
Table schema:
```
alerts(device_id, ts, kind, severity, message, reading_id)
```

##### API
```ts
// src/features/alerts/api.ts
import { supabase } from '@/lib/supabase';

export async function listAlerts(deviceId: string, page=0, pageSize=50) {
  const { data, error } = await supabase
    .from('alerts')
    .select('*')
    .eq('device_id', deviceId)
    .order('ts', { ascending: false })
    .range(page*pageSize, page*pageSize + pageSize - 1);
  if (error) throw error;
  return data;
}
```

---

## License
MIT © 2025 SwineTrack Team
