#pragma once
#include <Wire.h>
#include <Adafruit_BME680.h>
#include <Adafruit_MLX90640.h>

namespace swt {

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
  json += "]}";
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
  return ratio > 1.5f;
}
}  // namespace swt
