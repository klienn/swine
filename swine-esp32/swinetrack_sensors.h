#pragma once
#include <Wire.h>
#include <Adafruit_BME680.h>
#include <Adafruit_MLX90640.h>

namespace swt {

constexpr float kAirQualityRatioThreshold = 1.5f;

inline void initI2CBus(int sda, int scl, uint32_t khz100 = 100000) {
  Wire.begin(sda, scl);
  Wire.setClock(khz100);
  Wire.setTimeOut(50);
  delay(50);
}

inline void setI2CClock(uint32_t hz) {
  Wire.setClock(hz);
}

inline bool initBME680(Adafruit_BME680& bme) {
  uint8_t addrUsed = 0;
  bool ok = bme.begin(0x76, &Wire);
  if (ok) addrUsed = 0x76;
  if (!ok) {
    ok = bme.begin(0x77, &Wire);
    if (ok) addrUsed = 0x77;
  }

  if (!ok) {
    Serial.println("BME680 not found @ 0x76/0x77");
    return false;
  }

  Serial.printf("BME680 OK @ 0x%02X\n", addrUsed);
  bme.setTemperatureOversampling(BME680_OS_8X);
  bme.setHumidityOversampling(BME680_OS_2X);
  bme.setPressureOversampling(BME680_OS_4X);
  bme.setGasHeater(320, 150);
  return true;
}

inline bool initMLX90640(Adafruit_MLX90640& mlx) {
  if (!mlx.begin(0x33, &Wire)) {
    Serial.println("MLX90640 not found @ 0x33");
    return false;
  }
  mlx.setMode(MLX90640_CHESS);
  mlx.setResolution(MLX90640_ADC_18BIT);
  mlx.setRefreshRate(MLX90640_2_HZ);
  return true;
}

inline bool readBME680(Adafruit_BME680& bme, float& tempC, float& hum, float& pressHpa, float& gasRes) {
  if (!bme.performReading()) return false;
  tempC = bme.temperature;
  hum = bme.humidity;
  pressHpa = bme.pressure / 100.0f;
  gasRes = bme.gas_resistance;
  return true;
}

inline bool readMLX90640(Adafruit_MLX90640& mlx, float* outFrame) {
  mlx.getFrame(outFrame);  // 32x24 floats
  return true;
}

inline String makeThermalJson(const float* frame, float& tMin, float& tMax, float& tAvg) {
  tMin = 1e9;
  tMax = -1e9;
  double sum = 0;
  for (int i = 0; i < 32 * 24; i++) {
    float v = frame[i];
    if (v < tMin) tMin = v;
    if (v > tMax) tMax = v;
    sum += v;
  }
  tAvg = sum / (32.0 * 24.0);

  String json;
  json.reserve(6000);
  json = "{\"w\":32,\"h\":24,\"data\":[";
  for (int i = 0; i < 32 * 24; i++) {
    json += String(frame[i], 2);
    if (i < 32 * 24 - 1) json += ",";
  }
  // add tMin, tMax, tAvg to JSON
  json += "],\"tMin\":" + String(tMin, 2)
       + ",\"tMax\":" + String(tMax, 2)
       + ",\"tAvg\":" + String(tAvg, 2)
       + "}";
  return json;
}

inline String makeThermalSummaryJson(const float* frame, float& tMin, float& tMax, float& tAvg) {
  tMin = 1e9;
  tMax = -1e9;
  double sum = 0;
  for (int i = 0; i < 32 * 24; i++) {
    float v = frame[i];
    if (v < tMin) tMin = v;
    if (v > tMax) tMax = v;
    sum += v;
  }
  tAvg = sum / (32.0 * 24.0);
  String s = "{\"w\":32,\"h\":24"
             ",\"tMin\":"
             + String(tMin, 2) + ",\"tMax\":" + String(tMax, 2) + ",\"tAvg\":" + String(tAvg, 2) + "}";
  return s;
}

inline String makeReadingJson(float tempC, float hum, float press, float gasRes, float iaq,
                              float tMin, float tMax, float tAvg) {
  String s = "{\"tempC\":" + String(tempC, 1)
             + ",\"humidity\":" + String(hum, 1)
             + ",\"pressure\":" + String(press, 1)
             + ",\"gasRes\":" + String(gasRes, 0)
             + ",\"iaq\":" + String(iaq, 1)
             + ",\"tMin\":" + String(tMin, 2)
             + ",\"tMax\":" + String(tMax, 2)
             + ",\"tAvg\":" + String(tAvg, 2)
             + "}";
  return s;
}

// simple VOC proxy via gasRes drop vs baseline

inline bool isAirQualityElevated(float gasRes, float& baseline) {
  if (baseline <= 0) baseline = gasRes;
  float ratio = baseline > 0 ? (baseline / gasRes) : 1.0f;
  return ratio > kAirQualityRatioThreshold;
}

inline void appendUint64(String& out, uint64_t value) {
  if (value == 0) {
    out += '0';
    return;
  }
  char buf[21];
  int i = 0;
  while (value > 0 && i < (int)sizeof(buf)) {
    buf[i++] = char('0' + (value % 10));
    value /= 10;
  }
  while (i > 0) {
    out += buf[--i];
  }
}

inline String makeAlertContextJson(float tempC, float hum, float press, float gasRes, float iaq,
                                   float snapTMin, float snapTMax, float snapTAvg, float prevTMax,
                                   float gasBaseline, float feverThresholdC,
                                   bool airQualityElevated, bool feverDetectedNow,
                                   bool feverDetectedAtTrigger, const char* triggerReason,
                                   uint32_t sensorSampleMs, uint64_t capturedAtMs,
                                   uint32_t uptimeMs) {
  const bool gasBaselineValid = gasBaseline > 0.0f;
  const float gasDelta = gasBaselineValid ? (gasBaseline - gasRes) : 0.0f;
  const float gasRatio = (gasBaselineValid && gasRes > 0.0f) ? (gasBaseline / gasRes) : 0.0f;
  const float feverDelta = snapTMax - feverThresholdC;
  const bool feverObserved = feverDetectedNow || feverDetectedAtTrigger;
  const bool hasSensorSample = sensorSampleMs > 0;
  const uint32_t sensorSampleAgeMs = hasSensorSample ? (uptimeMs - sensorSampleMs) : 0;
  uint64_t sensorSampleCapturedAtMs = 0;
  if (hasSensorSample && capturedAtMs >= sensorSampleAgeMs) {
    sensorSampleCapturedAtMs = capturedAtMs - (uint64_t)sensorSampleAgeMs;
  }

  String s;
  s.reserve(768);
  s = "{\"tempC\":" + String(tempC, 1);
  s += ",\"humidity\":" + String(hum, 1);
  s += ",\"pressure\":" + String(press, 1);
  s += ",\"gasRes\":" + String(gasRes, 0);
  s += ",\"iaq\":" + String(iaq, 1);
  s += ",\"tMin\":" + String(snapTMin, 2);
  s += ",\"tMax\":" + String(snapTMax, 2);
  s += ",\"tAvg\":" + String(snapTAvg, 2);
  s += ",\"prevTMax\":" + String(prevTMax, 2);
  s += ",\"gasBaseline\":" + String(gasBaseline, 0);
  s += ",\"gasBaselineValid\":";
  s += gasBaselineValid ? "true" : "false";
  s += ",\"gasDelta\":" + String(gasDelta, 0);
  s += ",\"gasRatio\":" + String(gasRatio, 2);
  s += ",\"airQualityRatioThreshold\":" + String(kAirQualityRatioThreshold, 2);
  s += ",\"airQualityElevated\":";
  s += airQualityElevated ? "true" : "false";
  s += ",\"feverThresholdC\":" + String(feverThresholdC, 1);
  s += ",\"feverDetected\":";
  s += feverDetectedNow ? "true" : "false";
  s += ",\"feverDetectedAtTrigger\":";
  s += feverDetectedAtTrigger ? "true" : "false";
  s += ",\"feverObserved\":";
  s += feverObserved ? "true" : "false";
  s += ",\"feverDelta\":" + String(feverDelta, 2);
  s += ",\"triggerFlags\":[";
  bool hasFlag = false;
  if (airQualityElevated) {
    s += "\"air\"";
    hasFlag = true;
  }
  if (feverObserved) {
    if (hasFlag) s += ",";
    s += "\"fever\"";
    hasFlag = true;
  }
  if (!hasFlag) {
    s += "\"unknown\"";
  }
  s += "]";
  s += ",\"triggerReason\":\"";
  if (triggerReason) s += triggerReason;
  s += "\"";
  s += ",\"capturedAtMs\":";
  appendUint64(s, capturedAtMs);
  s += ",\"sensorSampleAvailable\":";
  s += hasSensorSample ? "true" : "false";
  s += ",\"sensorSampleUptimeMs\":" + String(sensorSampleMs);
  s += ",\"sensorSampleAgeMs\":" + String(sensorSampleAgeMs);
  s += ",\"sensorSampleCapturedAtMs\":";
  appendUint64(s, sensorSampleCapturedAtMs);
  s += ",\"uptimeMs\":" + String(uptimeMs);
  s += ",\"source\":\"esp32\"";
  s += ",\"snapshot\":true";
  s += "}";
  return s;
}
}  // namespace swt
