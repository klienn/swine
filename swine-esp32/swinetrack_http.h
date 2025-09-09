#pragma once
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <time.h>
#include <vector>
#include "mbedtls/md.h"
#include <ArduinoJson.h>
#include "swt_certs.h"

// ====== BUILD SWITCHES ======
// #define SWT_DEV_INSECURE 1          // DEV ONLY. Use CA pinning in production.
#define SWT_POST_VIA_HTTPCLIENT 1      // Use HTTPClient (stable). Set to 0 to try raw socket.

// ====== NAMESPACE ======
namespace swt {

// ---------- Time helpers ----------
inline void syncTime() {
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  time_t now = time(nullptr);
  while (now < 1700000000) { delay(200); now = time(nullptr); }
  Serial.printf("Time synced: %ld\n", (long)now);
}
inline uint64_t nowMs() { return (uint64_t)time(nullptr) * 1000ULL; }

// ---------- Wi-Fi helpers (with timeout + auto-reboot) ----------
inline bool connectWifiOnce(const char* ssid, const char* pass, uint32_t timeoutMs = 20000) {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, pass);
  Serial.print("WiFi");
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - t0) < timeoutMs) { delay(250); Serial.print("."); }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("IP: %s\n", WiFi.localIP().toString().c_str());
    return true;
  }
  Serial.println("[WiFi] connect timeout");
  return false;
}

inline void ensureWifiOrReboot(const char* ssid, const char* pass, uint32_t timeoutMs = 15000) {
  if (WiFi.status() == WL_CONNECTED) return;
  Serial.println("[WiFi] reconnecting...");
  WiFi.disconnect(true, true);
  delay(200);
  if (!connectWifiOnce(ssid, pass, timeoutMs)) {
    Serial.println("[WiFi] giving up, rebooting...");
    delay(500);
    ESP.restart();
  }
}

// ---------- HMAC helpers ----------
inline String toHex(const uint8_t* buf, size_t len) {
  static const char* hexmap = "0123456789abcdef";
  String s; s.reserve(len * 2);
  for (size_t i = 0; i < len; i++) { s += hexmap[(buf[i] >> 4) & 0xF]; s += hexmap[buf[i] & 0xF]; }
  return s;
}
inline String sha256Hex(const uint8_t* data, size_t len) {
  uint8_t out[32];
  mbedtls_md_context_t ctx; mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 0);
  mbedtls_md_starts(&ctx); mbedtls_md_update(&ctx, data, len); mbedtls_md_finish(&ctx, out);
  mbedtls_md_free(&ctx);
  return toHex(out, 32);
}
inline String hmacSha256Hex(const String& key, const String& msg) {
  uint8_t out[32];
  mbedtls_md_context_t ctx; mbedtls_md_init(&ctx);
  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  mbedtls_md_setup(&ctx, info, 1);
  mbedtls_md_hmac_starts(&ctx, (const unsigned char*)key.c_str(), key.length());
  mbedtls_md_hmac_update(&ctx, (const unsigned char*)msg.c_str(), msg.length());
  mbedtls_md_hmac_finish(&ctx, out);
  mbedtls_md_free(&ctx);
  return toHex(out, 32);
}

// ---------- URL helpers ----------
inline String _hostFromBase(const char* fnBase) {
  String s(fnBase); s.trim();
  int p = s.indexOf("://"); if (p >= 0) s.remove(0, p + 3);
  int slash = s.indexOf('/'); if (slash >= 0) s = s.substring(0, slash);
  return s;
}
inline String _basePathFromBase(const char* fnBase) {
  String s(fnBase); s.trim();
  int p = s.indexOf("://"); if (p >= 0) s.remove(0, p + 3);
  int slash = s.indexOf('/'); 
  return (slash >= 0) ? s.substring(slash) : String("");
}

// ---------- Camera fetch (tight, no keepalive) ----------
inline bool fetchCamera(const String& cameraUrl, std::vector<uint8_t>& outJpeg) {
  HTTPClient http;
  WiFiClient client;
  http.setReuse(false);            // do not keep connections
  http.setTimeout(15000);
  if (!http.begin(client, cameraUrl)) return false;
  int code = http.GET();
  if (code != 200) { http.end(); return false; }

  outJpeg.clear(); outJpeg.shrink_to_fit();
  WiFiClient* s = http.getStreamPtr();
  uint8_t buf[1024];
  while (http.connected()) {
    size_t n = s->readBytes(buf, sizeof(buf));
    if (!n) break;
    outJpeg.insert(outJpeg.end(), buf, buf + n);
    yield();
  }
  http.end();
  return outJpeg.size() > 0;
}

// ---------- raw-socket helpers (kept for option) ----------
inline bool _writeAll(WiFiClient& c, const uint8_t* buf, size_t len, uint32_t timeoutMs = 60000) {
  const size_t CHUNK = 1024;
  uint32_t t0 = millis();
  size_t sent = 0;
  while (sent < len) {
    size_t toSend = len - sent; if (toSend > CHUNK) toSend = CHUNK;
    int n = c.write(buf + sent, toSend);
    if (n < 0) return false;
    if (n == 0) {
      if (millis() - t0 > timeoutMs) return false;
      delay(1); continue;
    }
    sent += (size_t)n;
    t0 = millis();
    yield();
  }
  return true;
}
inline int _readHttpStatusAndDrain(WiFiClient& c, uint32_t timeoutMs = 60000) {
  c.setTimeout(timeoutMs);
  auto readLine = [&](){ String s = c.readStringUntil('\n'); if (s.length()) s.trim(); return s; };
  auto parse    = [&](const String& l){ int a=l.indexOf(' '), b=l.indexOf(' ',a+1); return (a>=0&&b>a)? l.substring(a+1,b).toInt():-1; };
  auto drainHdr = [&](){ for(;;){ String h=readLine(); if (h.length()==0) break; } };

  String l = readLine(); if (!l.length()) return -1;
  int code = parse(l);
  while (code == 100) { drainHdr(); l = readLine(); if (!l.length()) return -1; code = parse(l); }
  drainHdr();

  // tiny body drain to release pbufs
  uint8_t tmp[512]; uint32_t t0 = millis();
  while (millis() - t0 < 200) {
    int a = c.available(); if (a <= 0) { delay(5); continue; }
    while (a > 0) { int r = c.read(tmp, (a > (int)sizeof(tmp)? (int)sizeof(tmp): a)); if (r <= 0) break; a -= r; }
  }
  return code;
}

// ---------- POST multipart (HTTPClient by default) ----------
inline bool postMultipart(const char* fnBase,
                          const char* deviceId,
                          const char* deviceSecret,
                          const char* endpoint,
                          const std::vector<uint8_t>& camJpeg,
                          const String& thermalJson,
                          const String& readingJson)
{
  const String boundary = "----swinetrack_" + String(millis());
  const String head =
    "--" + boundary + "\r\n"
    "Content-Disposition: form-data; name=\"cam\"; filename=\"cam.jpg\"\r\n"
    "Content-Type: image/jpeg\r\n\r\n";
  String mid =
    "\r\n--" + boundary + "\r\n"
    "Content-Disposition: form-data; name=\"thermal\"; filename=\"thermal.json\"\r\n"
    "Content-Type: application/json\r\n\r\n" + thermalJson + "\r\n";
  String tail;
  if (readingJson.length()) {
    tail += "--" + boundary + "\r\n"
            "Content-Disposition: form-data; name=\"reading\"; filename=\"reading.json\"\r\n"
            "Content-Type: application/json\r\n\r\n" + readingJson + "\r\n";
  }
  tail += "--" + boundary + "--\r\n";

  const size_t contentLen = head.length() + camJpeg.size() + mid.length() + tail.length();

  // HMAC over exact bytes (we’ll build the same order below)
  String bodyHash;
  {
    mbedtls_md_context_t ctx; mbedtls_md_init(&ctx);
    const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    mbedtls_md_setup(&ctx, info, 0);
    mbedtls_md_starts(&ctx);
    mbedtls_md_update(&ctx, (const unsigned char*)head.c_str(), head.length());
    if (!camJpeg.empty()) mbedtls_md_update(&ctx, (const unsigned char*)camJpeg.data(), camJpeg.size());
    mbedtls_md_update(&ctx, (const unsigned char*)mid.c_str(),  mid.length());
    mbedtls_md_update(&ctx, (const unsigned char*)tail.c_str(), tail.length());
    uint8_t out[32]; mbedtls_md_finish(&ctx, out); mbedtls_md_free(&ctx);
    bodyHash = toHex(out, 32);
  }
  const String ts   = String((uint64_t)nowMs());
  const String path = _basePathFromBase(fnBase) + String(endpoint);
  const String base = "POST\n" + path + "\n" + bodyHash + "\n" + ts;
  const String sig  = hmacSha256Hex(deviceSecret, base);

#if SWT_POST_VIA_HTTPCLIENT
  // -------- Robust HTTPClient path (no response read, no reuse) --------
  Serial.printf("[HTTP] POST %s via HTTPClient len=%u\n", endpoint, (unsigned)contentLen);

  // Build a contiguous body (~10 KB)
  std::vector<uint8_t> body; body.reserve(contentLen);
  body.insert(body.end(), (const uint8_t*)head.c_str(), (const uint8_t*)head.c_str() + head.length());
  body.insert(body.end(), camJpeg.begin(), camJpeg.end());
  body.insert(body.end(), (const uint8_t*)mid.c_str(),  (const uint8_t*)mid.c_str()  + mid.length());
  body.insert(body.end(), (const uint8_t*)tail.c_str(), (const uint8_t*)tail.c_str() + tail.length());

  HTTPClient http;
  WiFiClientSecure client;
#ifdef SWT_DEV_INSECURE
  client.setInsecure();
#else
  client.setCACert(SUPABASE_ROOT_CA);
#endif
  client.setTimeout(120000);
  http.setReuse(false);                 // absolutely no keep-alive reuse
  http.useHTTP10(true);                 // simple response framing
  http.setConnectTimeout(20000);
  http.setTimeout(120000);

  String url = String(fnBase) + endpoint;
  if (!http.begin(client, url)) return false;
  http.setUserAgent("SwineTrack-ESP32/1.0");
  http.addHeader("Accept", "*/*");
  http.addHeader("Connection", "close");
  http.addHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
  http.addHeader("X-Device-Id", deviceId);
  http.addHeader("X-Timestamp", ts);
  http.addHeader("X-Signature", sig);

  int code = http.POST(body.data(), body.size());
  // Do NOT call getString() (avoid extra lwIP work). Just end().
  http.end();

  // Free the big body right after use
  // (reduces heap pressure before next camera fetch)
  // Note: vector goes out of scope now; it releases memory.
  Serial.printf("[HTTP] %s -> %d (%s)\n", endpoint, code, HTTPClient::errorToString(code).c_str());
  return (code >= 200 && code < 300);

#else
  // -------- Raw socket path (kept as an option) --------
  const String host     = _hostFromBase(fnBase);
  const String urlPath  = path;

  WiFiClientSecure c;
#ifdef SWT_DEV_INSECURE
  c.setInsecure();
#else
  c.setCACert(SUPABASE_ROOT_CA);
#endif
  c.setTimeout(60000);
  if (!c.connect(host.c_str(), 443)) { Serial.println("[HTTP] TLS connect failed"); return false; }

  c.printf("POST %s HTTP/1.1\r\n", urlPath.c_str());
  c.printf("Host: %s\r\n", host.c_str());
  c.print  ("User-Agent: SwineTrack-ESP32/1.0\r\n");
  c.printf("Content-Type: multipart/form-data; boundary=%s\r\n", boundary.c_str());
  c.printf("Content-Length: %u\r\n", (unsigned)contentLen);
  c.print  ("Connection: close\r\n");
  c.printf ("X-Device-Id: %s\r\n", deviceId);
  c.printf ("X-Timestamp: %s\r\n", ts.c_str());
  c.printf ("X-Signature: %s\r\n\r\n", sig.c_str());

  if (!_writeAll(c, (const uint8_t*)head.c_str(), head.length())) { c.stop(); return false; }
  if (!camJpeg.empty() && !_writeAll(c, camJpeg.data(), camJpeg.size())) { c.stop(); return false; }
  if (!_writeAll(c, (const uint8_t*)mid.c_str(),  mid.length()))        { c.stop(); return false; }
  if (!_writeAll(c, (const uint8_t*)tail.c_str(), tail.length()))       { c.stop(); return false; }

  int code = _readHttpStatusAndDrain(c, 60000);
  delay(20);
  c.stop();
  Serial.printf("[HTTP] %s -> %d\n", endpoint, code);
  return (code >= 200 && code < 300);
#endif
}

// ---------- GET /config ----------
inline bool fetchConfig(const char* fnBase,
                        const char* deviceId,
                        const char* deviceSecret,
                        String& inoutCameraUrl,
                        uint32_t& inoutLiveMs,
                        uint32_t& inoutReadMs,
                        float& inoutOverlayAlpha,
                        float& inoutFeverC)
{
  String url  = String(fnBase); url.trim(); url += "/config";
  String path = "/config";

  String ts = String((uint64_t)nowMs());
  String bodyHash = sha256Hex((const uint8_t*)"", 0);
  String base = "GET\n" + path + "\n" + bodyHash + "\n" + ts;
  String sig  = hmacSha256Hex(deviceSecret, base);

  HTTPClient http;
  WiFiClientSecure client;
#ifdef SWT_DEV_INSECURE
  client.setInsecure();
#else
  client.setCACert(SUPABASE_ROOT_CA);
#endif
  client.setTimeout(60000);
  http.setReuse(false);
  http.useHTTP10(true);
  http.setConnectTimeout(20000);
  http.setTimeout(60000);

  Serial.println(F("[HTTP] fetchConfig using WiFiClientSecure"));
  if (!http.begin(client, url)) { Serial.println(F("[HTTP] begin() failed")); return false; }
  http.setUserAgent("SwineTrack-ESP32/1.0");
  http.addHeader("X-Device-Id", deviceId);
  http.addHeader("X-Timestamp", ts);
  http.addHeader("X-Signature", sig);

  int code = http.GET();
  String resp;
  if (code > 0) resp = http.getString();  // small JSON body; ok to read
  http.end();

  Serial.printf("[HTTP] /config -> %d\n", code);
  if (code < 200 || code >= 300) { Serial.printf("GET /config -> %d %s\n", code, resp.c_str()); return false; }

  DynamicJsonDocument doc(1024);
  auto err = deserializeJson(doc, resp);
  if (err) { Serial.printf("config parse err: %s\n", err.c_str()); return false; }

  if (doc["camera_url"].is<const char*>())
    inoutCameraUrl = String((const char*)doc["camera_url"]);

  if (doc["config"].is<JsonObject>()) {
    JsonObject cfg = doc["config"].as<JsonObject>();
    if (cfg["live_frame_interval_s"].is<float>()) inoutLiveMs      = (uint32_t)(cfg["live_frame_interval_s"].as<float>() * 1000);
    if (cfg["reading_interval_s"].is<float>())    inoutReadMs       = (uint32_t)(cfg["reading_interval_s"].as<float>() * 1000);
    if (cfg["overlay_alpha"].is<float>())         inoutOverlayAlpha = cfg["overlay_alpha"].as<float>();
    if (cfg["fever_c"].is<float>())               inoutFeverC       = cfg["fever_c"].as<float>();
  }
  return true;
}

// ---------- Tiny ping (optional) ----------
inline bool postPing(const char* fnBase) {
  String host = _hostFromBase(fnBase);
  String path = _basePathFromBase(fnBase) + "/ping";

  WiFiClientSecure c;
#ifdef SWT_DEV_INSECURE
  c.setInsecure();
#else
  c.setCACert(SUPABASE_ROOT_CA);
#endif
  c.setTimeout(30000);

  if (!c.connect(host.c_str(), 443)) { Serial.println("[PING] TLS connect failed"); return false; }

  const char* body = "hello"; size_t len = strlen(body);
  c.printf("POST %s HTTP/1.1\r\n", path.c_str());
  c.printf("Host: %s\r\n", host.c_str());
  c.print  ("User-Agent: SwineTrack-ESP32/1.0\r\n");
  c.print  ("Content-Type: text/plain\r\n");
  c.printf ("Content-Length: %u\r\n", (unsigned)len);
  c.print  ("Connection: close\r\n\r\n");
  c.print  (body);

  int code = _readHttpStatusAndDrain(c, 30000);
  delay(10);
  c.stop();
  Serial.printf("[PING] -> %d\n", code);
  return (code >= 200 && code < 300);
}

} // namespace swt
