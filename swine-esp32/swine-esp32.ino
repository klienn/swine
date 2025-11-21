#include "swinetrack_http.h"
#include "swinetrack_sensors.h"
#include "async_http_uploader.h"
#include "swinetrack_local_server.h"

using namespace swt;

const char* WIFI_SSID = "MERCUSYS_0D8A";
const char* WIFI_PASS = "22854186";
const char* FN_BASE = "https://tqhbmujdtqxqivaesydq.functions.supabase.co";
const char* DEVICE_ID = "798d7d0b-965c-4eff-ba65-ce081bc139eb";
const char* DEVICE_SECRET = "05d35d61907b85a1422636bc2518eea0e3e0e72342e32a2cba02505a313ed379";
const char* LIVE_CAMERA_ENDPOINT = "/ingest-live-frame";
const char* LIVE_THERMAL_ENDPOINT = "/ingest-live-thermal";

String CAMERA_URL = "http://cam-pen1.local/capture?res=VGA";
uint32_t LIVE_FRAME_INTERVAL_MS = 1000;
uint32_t READING_INTERVAL_MS = 180000;
const uint32_t SNAPSHOT_COOLDOWN_MS = 5UL * 60UL * 1000UL;
uint32_t REALTIME_THERMAL_INTERVAL_MS = 50;
float OVERLAY_ALPHA = 0.35;
float FEVER_C = 39.5;
const uint16_t THERMAL_STREAM_PORT = 8787;

struct ThermalViewState {
  bool valid;
  float tMin;
  float tMax;
  float tAvg;
  uint64_t capturedAtMs;
};

static ThermalViewState gLatestThermalView = {false, 0.0f, 0.0f, 0.0f, 0};

static inline void recordLatestThermalView(float tMin, float tMax, float tAvg,
                                          uint64_t capturedAtMs) {
  gLatestThermalView.valid = true;
  gLatestThermalView.tMin = tMin;
  gLatestThermalView.tMax = tMax;
  gLatestThermalView.tAvg = tAvg;
  gLatestThermalView.capturedAtMs = capturedAtMs;
}

Adafruit_BME680 bme;
Adafruit_MLX90640 mlx;
float mlxFrame[32 * 24];
float gasBaseline = 0;

uint32_t lastLive = 0, lastReading = 0, lastRealtimeThermal = 0;
bool gCloudOnline = false;
bool gConfigFetched = false;
uint32_t gLastTimeSyncAttemptMs = 0;
uint32_t gLastConfigAttemptMs = 0;
const uint32_t CLOUD_SYNC_RETRY_MS = 60000;
const uint32_t CONFIG_FETCH_RETRY_MS = 120000;

// One background uploader for all endpoints
AsyncUploader uploader(FN_BASE, DEVICE_ID, DEVICE_SECRET);

void setup() {
  Serial.begin(115200);
  delay(150);

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.setHostname("esp32main");

  ensureWifiOrReboot(WIFI_SSID, WIFI_PASS, 20000);
  gLastTimeSyncAttemptMs = millis();
  gCloudOnline = syncTime(20000);
  if (!gCloudOnline) {
    Serial.println("[setup] time sync failed; running in local-only mode");
  }

  initI2CBus(21, 22, 100000);
  initBME680(bme);
  initMLX90640(mlx);
  setI2CClock(400000);

  if (gCloudOnline) {
    Serial.println("[setup] fetching remote config...");
    gLastConfigAttemptMs = millis();
    gConfigFetched = fetchConfig(FN_BASE, DEVICE_ID, DEVICE_SECRET,
                                 CAMERA_URL, LIVE_FRAME_INTERVAL_MS, READING_INTERVAL_MS,
                                 OVERLAY_ALPHA, FEVER_C);
    if (!gConfigFetched) {
      Serial.println("[setup] config fetch failed; keeping defaults");
    }
  } else {
    Serial.println("[setup] skipping remote config fetch (offline)");
  }

  Serial.printf("FN_BASE: '%s'\n", FN_BASE);
  Serial.printf("Config: live=%lums read=%lums alpha=%.2f fever=%.1f cam=%s\n",
                LIVE_FRAME_INTERVAL_MS, READING_INTERVAL_MS, OVERLAY_ALPHA, FEVER_C,
                CAMERA_URL.c_str());

  uploader.begin(24576);
  swt::startRealtimeThermalServer(THERMAL_STREAM_PORT);
}

void loop() {
  const uint32_t now = millis();
  swt::serviceRealtimeThermalServer();

  if (now - lastRealtimeThermal >= REALTIME_THERMAL_INTERVAL_MS) {
    float rtMin = 0, rtMax = 0, rtAvg = 0;
    readMLX90640(mlx, mlxFrame);
    String realtimeJson = makeThermalJson(mlxFrame, rtMin, rtMax, rtAvg);
    const uint64_t realtimeCapturedAt = swt::nowMs();
    swt::publishRealtimeThermal(realtimeJson, rtMin, rtMax, rtAvg, realtimeCapturedAt);
    recordLatestThermalView(rtMin, rtMax, rtAvg, realtimeCapturedAt);
    lastRealtimeThermal = now;
  }

  // Wi-Fi guard
  static uint32_t lastGuard = 0;
  if (now - lastGuard >= 10000) {
    ensureWifiOrReboot(WIFI_SSID, WIFI_PASS, 10000);
    if (WiFi.status() == WL_CONNECTED) {
      if (!gCloudOnline && (now - gLastTimeSyncAttemptMs) >= CLOUD_SYNC_RETRY_MS) {
        Serial.println("[net] retrying time sync...");
        gLastTimeSyncAttemptMs = now;
        gCloudOnline = syncTime(5000);
        if (gCloudOnline) {
          Serial.println("[net] time sync restored; enabling cloud features");
          gConfigFetched = false;  // force config refresh
        }
      }
      if (gCloudOnline && !gConfigFetched &&
          (now - gLastConfigAttemptMs) >= CONFIG_FETCH_RETRY_MS) {
        Serial.println("[net] fetching remote config...");
        gLastConfigAttemptMs = now;
        gConfigFetched = fetchConfig(FN_BASE, DEVICE_ID, DEVICE_SECRET,
                                     CAMERA_URL, LIVE_FRAME_INTERVAL_MS, READING_INTERVAL_MS,
                                     OVERLAY_ALPHA, FEVER_C);
        if (!gConfigFetched) {
          Serial.println("[net] config fetch failed; will retry");
        }
      }
    }
    lastGuard = now;
  }
  if (WiFi.status() != WL_CONNECTED) {
    delay(50);
    return;
  }

  // Cached scalars / thermal summary
  static float lastTemp = 0, lastHum = 0, lastPress = 0, lastGas = 0, lastIAQ = 0;
  static float tMin = 0, tMax = 0, tAvg = 0;

  // Periodic sensor read
  if (now - lastReading >= READING_INTERVAL_MS) {
    if (readBME680(bme, lastTemp, lastHum, lastPress, lastGas)) {
      lastIAQ = 0;
      readMLX90640(mlx, mlxFrame);
      String _tmp = makeThermalSummaryJson(mlxFrame, tMin, tMax, tAvg);  // just to refresh stats
      _tmp = String();
      lastReading = now;
    }
  }

  // Camera fetch backoff
  static uint8_t camFailCount = 0;
  static uint32_t camBackoffUntil = 0;

  // --- near-live frame push (summary thermal; full grid NOT needed) ---
  if (now - lastLive >= LIVE_FRAME_INTERVAL_MS) {
    float liveTMin = 0, liveTMax = 0, liveTAvg = 0;
    readMLX90640(mlx, mlxFrame);
    String thJson = makeThermalJson(mlxFrame, liveTMin, liveTMax, liveTAvg);
    const uint64_t liveCapturedAt = swt::nowMs();
    swt::publishRealtimeThermal(thJson, liveTMin, liveTMax, liveTAvg, liveCapturedAt);
    recordLatestThermalView(liveTMin, liveTMax, liveTAvg, liveCapturedAt);
    lastRealtimeThermal = now;
    if (gCloudOnline) {
      String rdJson = makeReadingJson(lastTemp, lastHum, lastPress, lastGas, lastIAQ,
                                      liveTMin, liveTMax, liveTAvg);

      if (uploader.enqueue(LIVE_THERMAL_ENDPOINT, std::vector<uint8_t>(), thJson, rdJson)) {
        Serial.printf("[loop] thermal sample enqueued @ %lu ms\n", (unsigned long)now);
      } else {
        Serial.println("[loop] thermal sample DROPPED (queue backpressure)");
      }

      const bool cameraAllowed = (camBackoffUntil == 0) || (now >= camBackoffUntil);
      if (cameraAllowed) {
        std::vector<uint8_t> jpeg;
        uint32_t tFetch0 = millis();
        bool camOk = swt::fetchCamera(CAMERA_URL, jpeg);
        uint32_t fetchMs = millis() - tFetch0;

        if (camOk) {
          Serial.printf("[loop] camera fetch ok in %lums, size=%u (heap=%u)\n",
                        (unsigned long)fetchMs, (unsigned)jpeg.size(), ESP.getFreeHeap());
          camFailCount = 0;
          camBackoffUntil = 0;

          if (uploader.enqueue(LIVE_CAMERA_ENDPOINT, std::move(jpeg), thJson, rdJson)) {
            Serial.printf("[loop] live frame enqueued @ %lu ms\n", (unsigned long)now);
          } else {
            Serial.println("[loop] live frame DROPPED (queue backpressure)");
          }
        } else {
          Serial.printf("[loop] camera fetch FAILED after %lums\n", (unsigned long)fetchMs);
          camFailCount = min<uint8_t>(camFailCount + 1, 6);
          camBackoffUntil = now + (1000UL << camFailCount);
        }
      }

      rdJson = String();
    }

    thJson = String();
    lastLive = now;
  }

  // --- alert snapshot (full thermal grid), with cooldown ---
  static uint32_t alertCooldownUntil = 0;
  static uint32_t lastSnapshotAtMs = 0;
  static bool snapshotTimersInitialized = false;
  if (!snapshotTimersInitialized) {
    lastSnapshotAtMs = now;
    alertCooldownUntil = now;
    snapshotTimersInitialized = true;
  }
  const bool airQualityElevated = isAirQualityElevated(lastGas, gasBaseline);
  const float recentThermalMax = gLatestThermalView.valid ? gLatestThermalView.tMax : tMax;
  const bool feverAtTrigger = recentThermalMax > FEVER_C;
  const bool cooldownExpired = now >= alertCooldownUntil;
  const bool detectionTriggered = airQualityElevated || feverAtTrigger;
  const bool periodicDue = cooldownExpired && ((now - lastSnapshotAtMs) >= SNAPSHOT_COOLDOWN_MS);
  if (gCloudOnline && cooldownExpired && (detectionTriggered || periodicDue)) {
    std::vector<uint8_t> jpeg;
    if (swt::fetchCamera(CAMERA_URL, jpeg)) {
      readMLX90640(mlx, mlxFrame);
      float _tMin, _tMax, _tAvg;
      String thJson = makeThermalJson(mlxFrame, _tMin, _tMax, _tAvg);
      const uint64_t snapshotCapturedAt = swt::nowMs();
      swt::publishRealtimeThermal(thJson, _tMin, _tMax, _tAvg, snapshotCapturedAt);
      recordLatestThermalView(_tMin, _tMax, _tAvg, snapshotCapturedAt);
      lastRealtimeThermal = now;
      const bool feverNow = _tMax > FEVER_C;
      const bool feverObserved = feverAtTrigger || feverNow;
      const bool triggeredByPeriodicOnly = periodicDue && !detectionTriggered;
      const char* triggerReason = triggeredByPeriodicOnly
                                    ? "periodic"
                                  : airQualityElevated && feverObserved
                                      ? "air+fever"
                                    : airQualityElevated ? "air"
                                    : feverObserved      ? "fever"
                                                         : "unknown";
      String rdJson = makeAlertContextJson(lastTemp, lastHum, lastPress, lastGas, lastIAQ,
                                           _tMin, _tMax, _tAvg, recentThermalMax, gasBaseline, FEVER_C,
                                           airQualityElevated, feverNow, feverAtTrigger,
                                           triggerReason, lastReading, snapshotCapturedAt, now);
      if (uploader.enqueuePriority("/ingest-snapshot", std::move(jpeg), thJson, rdJson)) {
        Serial.printf("[loop] snapshot queued (alert priority, reason=%s, gas=%.2f baseline=%.2f, prevMax=%.2f thresh=%.1f, newMax=%.2f)\n",
                      triggerReason, lastGas, gasBaseline, recentThermalMax, FEVER_C, _tMax);
        alertCooldownUntil = now + SNAPSHOT_COOLDOWN_MS;
        lastSnapshotAtMs = now;
      } else {
        Serial.printf("[loop] snapshot enqueue FAILED (alert priority, reason=%s, gas=%.2f baseline=%.2f, prevMax=%.2f thresh=%.1f, newMax=%.2f)\n",
                      triggerReason, lastGas, gasBaseline, recentThermalMax, FEVER_C, _tMax);
      }
      thJson = String();
      rdJson = String();
    }
  }

  delay(3);
}
