#pragma once
#include <WiFi.h>

namespace swt {
namespace detail {

constexpr uint8_t kMaxRealtimeClients = 4;
constexpr uint32_t kRealtimeKeepAliveMs = 5000;

struct RealtimeClientSlot {
  WiFiClient client;
  bool inUse = false;
  uint32_t lastSendMs = 0;
  uint32_t lastSeqSent = 0;
};

struct RealtimeServerState {
  WiFiServer* server = nullptr;
  bool running = false;
  uint16_t port = 0;
  RealtimeClientSlot slots[kMaxRealtimeClients];
  String latestPayload;
  String latestSsePacket;
  uint32_t latestSeq = 0;
};

inline RealtimeServerState& state() {
  static RealtimeServerState s;
  return s;
}

inline void appendUint64(String& out, uint64_t value) {
  if (value == 0) {
    out += '0';
    return;
  }
  char buf[21];
  size_t i = 0;
  while (value > 0 && i < sizeof(buf)) {
    buf[i++] = char('0' + (value % 10));
    value /= 10;
  }
  while (i > 0) {
    out += buf[--i];
  }
}

inline void sendTextHttpResponse(WiFiClient& client, const char* statusLine,
                                 const char* contentType, const String& body,
                                 bool keepAlive = false) {
  client.print(statusLine);
  client.print("\r\nAccess-Control-Allow-Origin: *");
  client.print("\r\nAccess-Control-Allow-Headers: Content-Type");
  client.print("\r\nCache-Control: no-cache");
  client.print(keepAlive ? "\r\nConnection: keep-alive" : "\r\nConnection: close");
  client.print("\r\nContent-Type: ");
  client.print(contentType);
  client.print("\r\nContent-Length: ");
  client.print(body.length());
  client.print("\r\n\r\n");
  if (body.length()) client.print(body);
  if (!keepAlive) client.stop();
}

inline void sendCorsPreflight(WiFiClient& client) {
  client.print("HTTP/1.1 204 No Content\r\n");
  client.print("Access-Control-Allow-Origin: *\r\n");
  client.print("Access-Control-Allow-Headers: Content-Type\r\n");
  client.print("Access-Control-Allow-Methods: GET, OPTIONS\r\n");
  client.print("Content-Length: 0\r\n\r\n");
  client.stop();
}

inline void closeSlot(RealtimeClientSlot& slot) {
  if (slot.inUse) {
    slot.client.stop();
    slot.inUse = false;
    slot.lastSendMs = 0;
    slot.lastSeqSent = 0;
  }
}

inline RealtimeClientSlot* reserveSlot() {
  auto& s = state();
  for (auto& slot : s.slots) {
    if (!slot.inUse) {
      slot.inUse = true;
      slot.lastSendMs = 0;
      slot.lastSeqSent = 0;
      return &slot;
    }
  }
  return nullptr;
}

inline void handleIncomingHttp(WiFiClient& client) {
  client.setTimeout(15);
  String requestLine = client.readStringUntil('\n');
  requestLine.trim();
  if (requestLine.length() == 0) {
    client.stop();
    return;
  }
  int sp1 = requestLine.indexOf(' ');
  int sp2 = requestLine.indexOf(' ', sp1 + 1);
  String method = (sp1 > 0) ? requestLine.substring(0, sp1) : String();
  String path = (sp1 > 0 && sp2 > sp1) ? requestLine.substring(sp1 + 1, sp2) : String("/");
  path.trim();
  int q = path.indexOf('?');
  if (q >= 0) path = path.substring(0, q);

  while (client.connected()) {
    String header = client.readStringUntil('\n');
    header.trim();
    if (header.length() == 0) break;
  }

  if (method.equalsIgnoreCase("OPTIONS")) {
    sendCorsPreflight(client);
    return;
  }
  if (!method.equalsIgnoreCase("GET")) {
    sendTextHttpResponse(client, "HTTP/1.1 405 Method Not Allowed", "text/plain",
                         "Method not allowed");
    return;
  }

  auto& s = state();
  if (path == "/" || path == "/status") {
    String body = "{\"status\":\"ok\",\"path\":\"/thermal/latest\",\"stream\":\"/thermal-stream\"}";
    sendTextHttpResponse(client, "HTTP/1.1 200 OK", "application/json", body);
    return;
  }
  if (path == "/thermal/latest" || path == "/thermal/latest/") {
    if (s.latestPayload.length() == 0) {
      sendTextHttpResponse(client, "HTTP/1.1 404 Not Found", "application/json",
                           "{\"error\":\"no-thermal-data\"}");
    } else {
      sendTextHttpResponse(client, "HTTP/1.1 200 OK", "application/json", s.latestPayload);
    }
    return;
  }
  if (path == "/thermal-stream" || path == "/thermal/stream") {
    RealtimeClientSlot* slot = reserveSlot();
    if (!slot) {
      sendTextHttpResponse(client, "HTTP/1.1 503 Service Unavailable",
                           "text/plain", "Too many subscribers");
      return;
    }
    slot->client = client;
    slot->client.print(
      "HTTP/1.1 200 OK\r\n"
      "Content-Type: text/event-stream\r\n"
      "Cache-Control: no-cache\r\n"
      "Connection: keep-alive\r\n"
      "Access-Control-Allow-Origin: *\r\n\r\n");
    slot->lastSendMs = millis();
    if (s.latestSeq != 0 && s.latestSsePacket.length()) {
      slot->client.print(s.latestSsePacket);
      slot->lastSeqSent = s.latestSeq;
    }
    return;
  }

  sendTextHttpResponse(client, "HTTP/1.1 404 Not Found", "text/plain", "Not found");
}

inline void pumpNewClients() {
  auto& s = state();
  if (!s.running || !s.server) return;
  uint8_t guard = 0;
  while (guard++ < 4) {
    WiFiClient client = s.server->available();
    if (!client) break;
    handleIncomingHttp(client);
  }
}

inline void pumpExistingClients() {
  auto& s = state();
  if (!s.running) return;
  const uint32_t now = millis();
  for (auto& slot : s.slots) {
    if (!slot.inUse) continue;
    if (!slot.client.connected()) {
      closeSlot(slot);
      continue;
    }
    if (s.latestSeq != 0 && slot.lastSeqSent != s.latestSeq &&
        s.latestSsePacket.length()) {
      slot.client.print(s.latestSsePacket);
      slot.lastSeqSent = s.latestSeq;
      slot.lastSendMs = now;
      continue;
    }
    if (now - slot.lastSendMs > kRealtimeKeepAliveMs) {
      slot.client.print(": keep-alive\n\n");
      slot.lastSendMs = now;
    }
  }
}

}  // namespace detail

inline void startRealtimeThermalServer(uint16_t port = 8787) {
  auto& s = detail::state();
  if (!s.server || s.port != port) {
    if (!s.server) {
      s.server = new WiFiServer(port);
    } else if (s.port != port) {
      delete s.server;
      s.server = new WiFiServer(port);
    }
  }
  s.port = port;
  s.server->begin();
  s.server->setNoDelay(true);
  s.running = true;
  String ip = WiFi.localIP().toString();
  Serial.printf("[rt-thermal] server at http://%s:%u (stream=/thermal-stream)\n",
                ip.c_str(), port);
}

inline void stopRealtimeThermalServer() {
  auto& s = detail::state();
  s.running = false;
  if (s.server) {
    s.server->close();
  }
  for (auto& slot : s.slots) detail::closeSlot(slot);
}

inline void serviceRealtimeThermalServer() {
  auto& s = detail::state();
  if (!s.running || !s.server) return;
  detail::pumpNewClients();
  detail::pumpExistingClients();
}

inline void publishRealtimeThermal(const String& thermalJson,
                                   float tMin, float tMax, float tAvg,
                                   uint64_t capturedAtMs) {
  auto& s = detail::state();
  s.latestPayload.reserve(thermalJson.length() + 96);
  s.latestPayload = "{\"capturedAt\":";
  detail::appendUint64(s.latestPayload, capturedAtMs);
  s.latestPayload += ",\"tMin\":" + String(tMin, 2);
  s.latestPayload += ",\"tMax\":" + String(tMax, 2);
  s.latestPayload += ",\"tAvg\":" + String(tAvg, 2);
  s.latestPayload += ",\"thermal\":";
  s.latestPayload += thermalJson;
  s.latestPayload += "}";

  s.latestSeq += 1;
  s.latestSsePacket.reserve(s.latestPayload.length() + 64);
  s.latestSsePacket = "event: thermal\nid: ";
  s.latestSsePacket += String(s.latestSeq);
  s.latestSsePacket += "\ndata: ";
  s.latestSsePacket += s.latestPayload;
  s.latestSsePacket += "\n\n";
}

inline uint16_t realtimeThermalPort() {
  return detail::state().port;
}

}  // namespace swt
