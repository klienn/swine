# SwineTrack – Right on Time, Healthy Swine

---

## Table of Contents

- [API Documentation](#api-documentation)
  - [1. Live](#1-live)
  - [2. Snapshots (Diary)](#2-snapshots-diary)
  - [3. Readings (History)](#3-readings-history)
  - [4. Alerts](#4-alerts)
- [License](#license)

---

## API Documentation

### 1. Live

Each device continuously overwrites:

```
frames-live/<device_id>/current.jpg (or .png)
```

#### API + Hook

```ts
// src/features/live/api.ts
import { supabase } from "@/lib/supabase";

export async function getLiveFrameUrl(deviceId: string, ttlSec = 10) {
  const jpg = await supabase.storage
    .from("frames-live")
    .createSignedUrl(`${deviceId}/current.jpg`, ttlSec);
  if (jpg.data?.signedUrl) return `${jpg.data.signedUrl}&cb=${Date.now()}`;
  const png = await supabase.storage
    .from("frames-live")
    .createSignedUrl(`${deviceId}/current.png`, ttlSec);
  if (png.data?.signedUrl) return `${png.data.signedUrl}&cb=${Date.now()}`;
  throw new Error("no_live_frame");
}
```

```ts
// src/features/live/useLiveFrame.ts
import { useEffect, useState } from "react";
import { getLiveFrameUrl } from "./api";

export function useLiveFrame(deviceId: string, intervalMs = 1000) {
  const [url, setUrl] = useState<string | null>(null);
  const [err, setErr] = useState<string | null>(null);

  useEffect(() => {
    let alive = true,
      timer: any;
    const tick = async () => {
      try {
        setUrl(await getLiveFrameUrl(deviceId, 10));
        setErr(null);
      } catch (e: any) {
        setErr(String(e?.message ?? e));
      } finally {
        if (alive) timer = setTimeout(tick, intervalMs);
      }
    };
    tick();
    return () => {
      alive = false;
      clearTimeout(timer);
    };
  }, [deviceId, intervalMs]);

  return { url, err };
}
```

#### Screen Example

```tsx
// src/screens/LiveScreen.tsx
import React from "react";
import { View, Image, ActivityIndicator, Text } from "react-native";
import { useDevice } from "@/context/DeviceContext";
import { useLiveFrame } from "@/features/live/useLiveFrame";

export default function LiveScreen() {
  const { deviceId } = useDevice();
  const { url, err } = useLiveFrame(deviceId!, 1000); // 1 fps polling

  if (!deviceId) return <Text>Select a device</Text>;
  if (!url) return <ActivityIndicator />;

  return (
    <View style={{ padding: 12 }}>
      <Image source={{ uri: url }} style={{ width: "100%", aspectRatio: 4 / 3 }} />
      {err ? <Text style={{ color: "orange", marginTop: 8 }}>warning: {err}</Text> : null}
    </View>
  );
}
```

---

### 2. Snapshots (Diary)

Database table:

```
snapshots(device_id, ts, overlay_path, reading_id, alert_id)
```

#### API

```ts
// src/features/snapshots/api.ts
import { supabase } from "@/lib/supabase";

export type SnapshotRow = {
  id: number;
  device_id: string;
  ts: string;
  overlay_path: string;
  reading_id: number | null;
  reading?: {
    ts: string;
    temp_c: number | null;
    humidity_rh: number | null;
    pressure_hpa: number | null;
    gas_res_ohm: number | null;
    iaq: number | null;
    t_min_c: number | null;
    t_max_c: number | null;
    t_avg_c: number | null;
  } | null;
};

export async function listSnapshots(deviceId: string, page = 0, pageSize = 20) {
  const { data, error } = await supabase
    .from("snapshots")
    .select(
      `
      id, device_id, ts, overlay_path, reading_id,
      reading:readings ( ts, temp_c, humidity_rh, pressure_hpa, gas_res_ohm, iaq, t_min_c, t_max_c, t_avg_c )
    `,
    )
    .eq("device_id", deviceId)
    .order("ts", { ascending: false })
    .range(page * pageSize, page * pageSize + pageSize - 1);

  if (error) throw error;

  return Promise.all(
    (data ?? []).map(async (row: any) => {
      const { data: s } = await supabase.storage
        .from("snapshots")
        .createSignedUrl(row.overlay_path, 60 * 30);
      return { ...row, imageUrl: s?.signedUrl ?? null };
    }),
  );
}
```

---

### 3. Readings (History)

Query last 7 days by default.

#### API

```ts
// src/features/readings/api.ts
import { supabase } from "@/lib/supabase";

export type ReadingRow = {
  id: number;
  device_id: string;
  ts: string;
  temp_c: number | null;
  humidity_rh: number | null;
  pressure_hpa: number | null;
  gas_res_ohm: number | null;
  iaq: number | null;
  t_min_c: number | null;
  t_max_c: number | null;
  t_avg_c: number | null;
};

export async function fetchReadings(
  deviceId: string,
  fromISO: string,
  toISO: string,
  limit = 1000,
) {
  const { data, error } = await supabase
    .from("readings")
    .select(
      "id, device_id, ts, temp_c, humidity_rh, pressure_hpa, gas_res_ohm, iaq, t_min_c, t_max_c, t_avg_c",
    )
    .eq("device_id", deviceId)
    .gte("ts", fromISO)
    .lte("ts", toISO)
    .order("ts", { ascending: false })
    .limit(limit);
  if (error) throw error;
  return data as ReadingRow[];
}
```

---

### 4. Alerts

Table schema:

```
alerts(device_id, ts, kind, severity, message, reading_id)
```

#### API

```ts
// src/features/alerts/api.ts
import { supabase } from "@/lib/supabase";

export async function listAlerts(deviceId: string, page = 0, pageSize = 50) {
  const { data, error } = await supabase
    .from("alerts")
    .select("*")
    .eq("device_id", deviceId)
    .order("ts", { ascending: false })
    .range(page * pageSize, page * pageSize + pageSize - 1);
  if (error) throw error;
  return data;
}
```

---

## License

MIT © 2025 SwineTrack Team
