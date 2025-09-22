#pragma once
#include <WiFiClientSecure.h>
#include <HTTPClient.h>  // for SUPABASE_ROOT_CA include path; not used for POST
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <Arduino.h>
#include <vector>
#include <algorithm>
#include "swinetrack_http.h"  // for toHex/hmac helpers + SUPABASE_ROOT_CA

namespace swt {

class AsyncUploader {
public:
  // One worker handles ANY endpoint; enqueue endpoint per item
  AsyncUploader(const char* fnBase,
                const char* deviceId,
                const char* deviceSecret,
                size_t queueLen = 6,
                uint8_t maxFailBeforeReset = 3)
    : _fnBase(fnBase),
      _deviceId(deviceId),
      _deviceSecret(deviceSecret),
      _maxFailBeforeReset(maxFailBeforeReset) {
    _q = xQueueCreate(queueLen, sizeof(UploadItem*));
    _host = _hostFromBase(_fnBase);
    _basePath = _basePathFromBase(_fnBase);
  }

  void begin(uint32_t taskStack = 24576) {
    if (_task) return;
    // Pin to core 1 (Arduino core). One network worker only.
    xTaskCreatePinnedToCore(taskTrampoline, "upld", taskStack, this, 1, &_task, /*core=*/1);
  }

  // Enqueue to a specific endpoint (e.g., "/ingest-live-frame" or "/ingest-snapshot")
  bool enqueue(const char* endpoint,
               std::vector<uint8_t>&& jpeg,
               const String& thermal,
               const String& reading) {
    if (!_q || !endpoint) return false;

    // Backpressure: if only 0–1 slots left, drop LIVE frames to keep things fresh
    UBaseType_t freeSlots = uxQueueSpacesAvailable(_q);
    if (freeSlots <= 1 && strcmp(endpoint, "/ingest-live-frame") == 0) {
      // Drop the frame silently (or log if you prefer)
      return false;
    }

    UploadItem* item = new UploadItem;
    item->endpoint = String(endpoint);
    item->jpeg = std::move(jpeg);
    item->thermal = thermal;
    item->reading = reading;
    item->tsMs = millis();
    if (xQueueSend(_q, &item, 0) != pdTRUE) {
      delete item;  // queue full; drop frame
      return false;
    }
    return true;
  }

  // Enqueue a priority item by first clearing the queue, then pushing to the front.
  // Used for alerts so they are delivered immediately.
  bool enqueuePriority(const char* endpoint,
                       std::vector<uint8_t>&& jpeg,
                       const String& thermal,
                       const String& reading) {
    if (!_q || !endpoint) return false;

    Serial.println("[upld] priority enqueue requested; clearing queue");
    clearQueue();  // drop pending uploads; alert takes precedence

    UploadItem* item = new UploadItem;
    item->endpoint = String(endpoint);
    item->jpeg = std::move(jpeg);
    item->thermal = thermal;
    item->reading = reading;
    item->tsMs = millis();
    if (xQueueSendToFront(_q, &item, 0) != pdTRUE) {
      delete item;
      return false;
    }
    return true;
  }


private:
  struct UploadItem {
    String endpoint;
    std::vector<uint8_t> jpeg;
    String thermal;
    String reading;
    uint32_t tsMs;
  };

  static void taskTrampoline(void* arg) {
    static_cast<AsyncUploader*>(arg)->run();
  }

  void run() {
    for (;;) {
      UploadItem* item = nullptr;
      if (xQueueReceive(_q, &item, 100 / portTICK_PERIOD_MS) != pdTRUE) {
        vTaskDelay(10 / portTICK_PERIOD_MS);
        continue;
      }

      // Drop stale *live* frames (>10s) to keep it “live”
      if (item->endpoint == "/ingest-live-frame" && (millis() - item->tsMs > 10000)) {
        delete item;
        continue;
      }

      int code = sendRawTLS(*item);
      const bool retry = (code < 0) || code == 429 || (code >= 500);

      if (retry) {
        _fail = std::min<uint8_t>(_fail + 1, 6);
        if (_fail >= _maxFailBeforeReset) {
          Serial.println("[upld] failure threshold reached, clearing queue");
          delete item;
          clearQueue();
          _fail = 0;
          vTaskDelay(1000 / portTICK_PERIOD_MS);
        } else {
          const uint32_t delayMs = (1000UL << _fail);
          Serial.printf("[upld] backoff %lums (code=%d, ep=%s)\n",
                        (unsigned long)delayMs, code, item->endpoint.c_str());
          requeueFront(item);
          vTaskDelay(delayMs / portTICK_PERIOD_MS);
        }
      } else {
        _fail = 0;
        delete item;
      }
    }
  }

  void requeueFront(UploadItem* item) {
    while (xQueueSendToFront(_q, &item, 0) != pdTRUE) {
      UploadItem* drop = nullptr;
      if (xQueueReceive(_q, &drop, 0) == pdTRUE) {
        delete drop;
      } else {
        break;
      }
    }
  }

  // --- Minimal-allocation HTTPS POST (HTTP/1.0 + close; stream parts directly) ---
  int sendRawTLS(const UploadItem& it) {
    // Build multipart boundaries/strings (small)
    const String boundary = "----swt_" + String(millis());

    const String head =
      "--" + boundary + "\r\n"
                        "Content-Disposition: form-data; name=\"cam\"; filename=\"cam.jpg\"\r\n"
                        "Content-Type: image/jpeg\r\n\r\n";

    String mid =
      "\r\n--" + boundary + "\r\n"
                            "Content-Disposition: form-data; name=\"thermal\"; filename=\"thermal.json\"\r\n"
                            "Content-Type: application/json\r\n\r\n"
      + it.thermal + "\r\n";

    String tail;
    if (it.reading.length()) {
      tail += "--" + boundary + "\r\n"
                                "Content-Disposition: form-data; name=\"reading\"; filename=\"reading.json\"\r\n"
                                "Content-Type: application/json\r\n\r\n"
              + it.reading + "\r\n";
    }
    tail += "--" + boundary + "--\r\n";

    // Pre-compute HMAC over the exact multipart bytes (head + jpeg + mid + tail)
    String bodyHash;
    {
      mbedtls_md_context_t ctx;
      mbedtls_md_init(&ctx);
      const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
      mbedtls_md_setup(&ctx, info, 0);
      mbedtls_md_starts(&ctx);
      mbedtls_md_update(&ctx, (const unsigned char*)head.c_str(), head.length());
      if (!it.jpeg.empty()) mbedtls_md_update(&ctx, it.jpeg.data(), it.jpeg.size());
      mbedtls_md_update(&ctx, (const unsigned char*)mid.c_str(), mid.length());
      mbedtls_md_update(&ctx, (const unsigned char*)tail.c_str(), tail.length());
      uint8_t out[32];
      mbedtls_md_finish(&ctx, out);
      mbedtls_md_free(&ctx);
      bodyHash = toHex(out, 32);
    }

    const String ts = String((uint64_t)nowMs());
    const String path = _basePath + it.endpoint;
    const String base = "POST\n" + path + "\n" + bodyHash + "\n" + ts;
    const String sig = hmacSha256Hex(_deviceSecret, base);

    const size_t contentLen = head.length() + it.jpeg.size() + mid.length() + tail.length();

    WiFiClientSecure c;
#ifdef SWT_DEV_INSECURE
    c.setInsecure();
#else
    c.setCACert(SUPABASE_ROOT_CA);
#endif
    c.setTimeout(60000);

    uint32_t t0 = millis();
    if (!c.connect(_host.c_str(), 443)) {
      Serial.println("[upld] TLS connect failed");
      return -1;
    }

    // HTTP/1.0 + close keeps things simple (no chunking, fixed length)
    c.printf("POST %s HTTP/1.0\r\n", path.c_str());
    c.printf("Host: %s\r\n", _host.c_str());
    c.print("User-Agent: SwineTrack-ESP32/1.0\r\n");
    c.printf("Content-Type: multipart/form-data; boundary=%s\r\n", boundary.c_str());
    c.printf("Content-Length: %u\r\n", (unsigned)contentLen);
    c.print("Connection: close\r\n");
    c.printf("X-Device-Id: %s\r\n", _deviceId);
    c.printf("X-Timestamp: %s\r\n", ts.c_str());
    c.printf("X-Signature: %s\r\n\r\n", sig.c_str());

    // Stream the body in parts (no giant buffer)
    if (!_writeAll(c, (const uint8_t*)head.c_str(), head.length())) {
      c.stop();
      return -1;
    }
    if (!it.jpeg.empty() && !_writeAll(c, it.jpeg.data(), it.jpeg.size())) {
      c.stop();
      return -1;
    }
    if (!_writeAll(c, (const uint8_t*)mid.c_str(), mid.length())) {
      c.stop();
      return -1;
    }
    if (!_writeAll(c, (const uint8_t*)tail.c_str(), tail.length())) {
      c.stop();
      return -1;
    }

    // Read status + drain small body
    int code = _readHttpStatusAndDrain(c, 60000);
    c.stop();

    uint32_t dt = millis() - t0;
    Serial.printf("[upld] %s -> %d (dt=%lums)\n", it.endpoint.c_str(), code, (unsigned long)dt);
    return code;
  }

  const char* _fnBase;
  const char* _deviceId;
  const char* _deviceSecret;

  // Cached URL parts
  String _host;
  String _basePath;  // may be "" for functions base

  TaskHandle_t _task = nullptr;
  QueueHandle_t _q = nullptr;
  uint8_t _fail = 0;
  uint8_t _maxFailBeforeReset = 0;

  void clearQueue() {
    if (!_q) return;
    UploadItem* drop = nullptr;
    while (xQueueReceive(_q, &drop, 0) == pdTRUE) {
      delete drop;
    }
  }
};

}  // namespace swt
