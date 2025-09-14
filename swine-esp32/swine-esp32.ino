// ==== SwiNetTrack — ESP32 Hub (minimal main) ====
#include "swinetrack_http.h"
#include "swinetrack_sensors.h"
#include "live_frame_uploader.h"

using namespace swt;  // just for brevity below

// ---- Project config (EDIT THESE) ----
const char* WIFI_SSID = "scam ni";
const char* WIFI_PASS = "Walakokabalo0123!";
const char* FN_BASE = "https://tqhbmujdtqxqivaesydq.functions.supabase.co";
const char* DEVICE_ID = "798d7d0b-965c-4eff-ba65-ce081bc139eb";
const char* DEVICE_SECRET = "05d35d61907b85a1422636bc2518eea0e3e0e72342e32a2cba02505a313ed379";

// Camera (mDNS or fixed IP). Can be overridden by /config on boot.
String CAMERA_URL = "http://cam-pen1.local/capture?res=VGA";

// Intervals & thresholds (overridden by /config if present)
uint32_t LIVE_FRAME_INTERVAL_MS = 500;
uint32_t READING_INTERVAL_MS = 60000;
float OVERLAY_ALPHA = 0.35;
float FEVER_C = 39.5;

// ---- Sensors & state ----
Adafruit_BME680 bme;
Adafruit_MLX90640 mlx;
float mlxFrame[32 * 24];
float gasBaseline = 0;

uint32_t lastReading = 0;

void setup() {
  Serial.begin(115200);
  delay(150);

  // Make Wi-Fi behavior predictable
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);

  // 1) Connect (with timeout -> reboot if it can’t)
  swt::ensureWifiOrReboot(WIFI_SSID, WIFI_PASS, 20000);

  // 2) NTP once (for HMAC timestamps)
  swt::syncTime();

  // 3) I2C + sensors
  swt::initI2CBus(21, 22, 100000);  // start slow
  swt::initBME680(bme);
  swt::initMLX90640(mlx);
  swt::setI2CClock(400000);  // then bump speed

  // 4) Remote config (camera url, intervals, thresholds)
  swt::fetchConfig(FN_BASE, DEVICE_ID, DEVICE_SECRET,
                   CAMERA_URL, LIVE_FRAME_INTERVAL_MS, READING_INTERVAL_MS,
                   OVERLAY_ALPHA, FEVER_C);

  startLiveFrameUploader(CAMERA_URL, FN_BASE, DEVICE_ID, DEVICE_SECRET, LIVE_FRAME_INTERVAL_MS);

  // (Optional) quick health check to the same domain
  // swt::postPing(FN_BASE);

  Serial.printf("FN_BASE: '%s'\n", FN_BASE);
  Serial.printf("Config: live=%lums read=%lums alpha=%.2f fever=%.1f cam=%s\n",
                LIVE_FRAME_INTERVAL_MS, READING_INTERVAL_MS, OVERLAY_ALPHA, FEVER_C,
                CAMERA_URL.c_str());
}


void loop() {
  const uint32_t now = millis();

  // --- keep Wi-Fi healthy every 10s (reboot if it can’t reconnect) ---
  static uint32_t lastGuard = 0;
  if (now - lastGuard >= 10000) {
    swt::ensureWifiOrReboot(WIFI_SSID, WIFI_PASS, 10000);
    lastGuard = now;
  }
  if (WiFi.status() != WL_CONNECTED) {
    // Give Wi-Fi a moment; guard will try again soon.
    delay(50);
    return;
  }

  // --- cached scalars / thermal summary ---
  static float lastTemp = 0, lastHum = 0, lastPress = 0, lastGas = 0, lastIAQ = 0;
  static float tMin = 0, tMax = 0, tAvg = 0;

  // --- periodic sensor read (BME + MLX summary) ---
  if (now - lastReading >= READING_INTERVAL_MS) {
    if (readBME680(bme, lastTemp, lastHum, lastPress, lastGas)) {
      lastIAQ = 0;                                                // TODO: replace with BSEC IAQ if you add it
      readMLX90640(mlx, mlxFrame);                                // fills mlxFrame
      String _tmp = makeThermalJson(mlxFrame, tMin, tMax, tAvg);  // only to compute stats
      _tmp = String();                                            // free temp String
      lastReading = now;
    }
  }

  // --- simple alert heuristic with cooldown ---
  static uint32_t alertCooldownUntil = 0;
  if ((now >= alertCooldownUntil) && (isAirQualityElevated(lastGas, gasBaseline) || tMax > FEVER_C)) {
    std::vector<uint8_t> jpeg;
    if (swt::fetchCamera(CAMERA_URL, jpeg)) {
      readMLX90640(mlx, mlxFrame);
      float _tMin, _tMax, _tAvg;
      String thJson = makeThermalJson(mlxFrame, _tMin, _tMax, _tAvg);
      String rdJson = makeReadingJson(lastTemp, lastHum, lastPress, lastGas, /*iaq*/ lastIAQ, _tMin, _tMax, _tAvg);

      bool ok = swt::postMultipart(FN_BASE, DEVICE_ID, DEVICE_SECRET,
                                   "/ingest-snapshot", jpeg, thJson, rdJson);
      if (!ok) {
        Serial.println("snapshot upload failed");
      } else {
        Serial.println("snapshot uploaded");
        alertCooldownUntil = now + 30000;  // 30s cooldown to avoid spamming
      }

      // free big buffers
      std::vector<uint8_t>().swap(jpeg);
      thJson = String();
      rdJson = String();
    }
  }

  // small yield to keep Wi-Fi/lwIP happy
  delay(3);
}
