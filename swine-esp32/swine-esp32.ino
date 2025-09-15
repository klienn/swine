#include "swinetrack_http.h"
#include "swinetrack_sensors.h"
#include "async_http_uploader.h"

using namespace swt;

const char* WIFI_SSID     = "scam ni";
const char* WIFI_PASS     = "Walakokabalo0123!";
const char* FN_BASE       = "https://tqhbmujdtqxqivaesydq.functions.supabase.co";
const char* DEVICE_ID     = "798d7d0b-965c-4eff-ba65-ce081bc139eb";
const char* DEVICE_SECRET = "05d35d61907b85a1422636bc2518eea0e3e0e72342e32a2cba02505a313ed379";

String   CAMERA_URL = "http://cam-pen1.local/capture?res=VGA";
uint32_t LIVE_FRAME_INTERVAL_MS = 1000;
uint32_t READING_INTERVAL_MS    = 60000;
float    OVERLAY_ALPHA          = 0.35;
float    FEVER_C                = 39.5;

Adafruit_BME680 bme;
Adafruit_MLX90640 mlx;
float mlxFrame[32 * 24];
float gasBaseline = 0;

uint32_t lastLive = 0, lastReading = 0;

// One background uploader for all endpoints
AsyncUploader uploader(FN_BASE, DEVICE_ID, DEVICE_SECRET);

void setup() {
  Serial.begin(115200);
  delay(150);

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);

  ensureWifiOrReboot(WIFI_SSID, WIFI_PASS, 20000);
  syncTime();

  initI2CBus(21, 22, 100000);
  initBME680(bme);
  initMLX90640(mlx);
  setI2CClock(400000);

  fetchConfig(FN_BASE, DEVICE_ID, DEVICE_SECRET,
              CAMERA_URL, LIVE_FRAME_INTERVAL_MS, READING_INTERVAL_MS,
              OVERLAY_ALPHA, FEVER_C);

  Serial.printf("FN_BASE: '%s'\n", FN_BASE);
  Serial.printf("Config: live=%lums read=%lums alpha=%.2f fever=%.1f cam=%s\n",
                LIVE_FRAME_INTERVAL_MS, READING_INTERVAL_MS, OVERLAY_ALPHA, FEVER_C,
                CAMERA_URL.c_str());

  uploader.begin(24576);
}

void loop() {
  const uint32_t now = millis();

  // Wi-Fi guard
  static uint32_t lastGuard = 0;
  if (now - lastGuard >= 10000) {
    ensureWifiOrReboot(WIFI_SSID, WIFI_PASS, 10000);
    lastGuard = now;
  }
  if (WiFi.status() != WL_CONNECTED) { delay(50); return; }

  // Cached scalars / thermal summary
  static float lastTemp = 0, lastHum = 0, lastPress = 0, lastGas = 0, lastIAQ = 0;
  static float tMin = 0, tMax = 0, tAvg = 0;

  // Periodic sensor read
  if (now - lastReading >= READING_INTERVAL_MS) {
    if (readBME680(bme, lastTemp, lastHum, lastPress, lastGas)) {
      lastIAQ = 0;
      readMLX90640(mlx, mlxFrame);
      String _tmp = makeThermalSummaryJson(mlxFrame, tMin, tMax, tAvg); // just to refresh stats
      _tmp = String();
      lastReading = now;
    }
  }

  // Camera fetch backoff
  static uint8_t  camFailCount = 0;
  static uint32_t camBackoffUntil = 0;
  if (camBackoffUntil && now < camBackoffUntil) {
    delay(10);
    return;
  }

  // --- near-live frame push (summary thermal; full grid NOT needed) ---
  if (now - lastLive >= LIVE_FRAME_INTERVAL_MS) {
    std::vector<uint8_t> jpeg;
    uint32_t tFetch0 = millis();
    bool camOk = swt::fetchCamera(CAMERA_URL, jpeg);
    uint32_t fetchMs = millis() - tFetch0;

    if (camOk) {
      Serial.printf("[loop] camera fetch ok in %lums, size=%u (heap=%u)\n",
                    (unsigned long)fetchMs, (unsigned)jpeg.size(), ESP.getFreeHeap());
      camFailCount = 0; camBackoffUntil = 0;

      readMLX90640(mlx, mlxFrame);
      // String thJson = makeThermalSummaryJson(mlxFrame, tMin, tMax, tAvg);
      String thJson = makeThermalJson(mlxFrame, tMin, tMax, tAvg);
      String rdJson = makeReadingJson(lastTemp, lastHum, lastPress, lastGas, lastIAQ, tMin, tMax, tAvg);

      if (uploader.enqueue("/ingest-live-frame", std::move(jpeg), thJson, rdJson)) {
        Serial.printf("[loop] live frame enqueued @ %lu ms\n", (unsigned long)now);
      } else {
        Serial.println("[loop] live frame DROPPED (queue backpressure)");
      }
      thJson = String(); rdJson = String();
    } else {
      Serial.printf("[loop] camera fetch FAILED after %lums\n", (unsigned long)fetchMs);
      camFailCount = min<uint8_t>(camFailCount + 1, 6);
      camBackoffUntil = now + (1000UL << camFailCount);
    }
    lastLive = now;
  }

  // --- alert snapshot (full thermal grid), with cooldown ---
  static uint32_t alertCooldownUntil = 0;
  if ((now >= alertCooldownUntil) && (isAirQualityElevated(lastGas, gasBaseline) || tMax > FEVER_C)) {
    std::vector<uint8_t> jpeg;
    if (swt::fetchCamera(CAMERA_URL, jpeg)) {
      readMLX90640(mlx, mlxFrame);
      float _tMin, _tMax, _tAvg;
      String thJson = makeThermalJson(mlxFrame, _tMin, _tMax, _tAvg);
      String rdJson = makeReadingJson(lastTemp, lastHum, lastPress, lastGas, lastIAQ, _tMin, _tMax, _tAvg);

      if (uploader.enqueue("/ingest-snapshot", std::move(jpeg), thJson, rdJson)) {
        Serial.println("[loop] snapshot queued (alert)");
        alertCooldownUntil = now + 30000;
      } else {
        Serial.println("[loop] snapshot enqueue FAILED");
      }
      thJson = String(); rdJson = String();
    }
  }

  delay(3);
}
