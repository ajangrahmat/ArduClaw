/**
 * ArduClaw v0.6 — Lightweight AI Automation Runtime for ESP32
 *
 * Features:
 *  - JSON serial protocol for WiFi + LLM configuration
 *  - WiFi credential storage in NVS (non-volatile storage)
 *  - LLM config storage in NVS (URL, API key, model)
 *  - HTTPS LLM API calls (WiFiClientSecure, insecure — skip cert verify)
 *  - Minimal web dashboard served on port 80
 *  - FreeRTOS WiFi task for background reconnect
 *
 * Serial Commands (JSON, newline-terminated):
 *  { "c": "ws",       "s": "SSID", "p": "password" }   → save + connect WiFi
 *  { "c": "wst" }                                        → WiFi status
 *  { "c": "wr"  }                                        → clear WiFi config
 *  { "c": "rst" }                                        → restart ESP32
 *  { "c": "st"  }                                        → full status
 *  { "c": "llm_set",  "url":"...", "key":"...", "model":"..." } → save LLM config
 *  { "c": "llm_get"  }                                   → read LLM config
 *  { "c": "llm_test" }                                   → test LLM (start server)
 *  { "c": "llm_chat", "prompt":"..." }                   → send prompt to LLM
 *  { "c": "start" }                                      → start web server
 *
 * Dependencies (install via Arduino Library Manager or PlatformIO):
 *  - ArduinoJson  >= 7.x     (bblanchon/ArduinoJson)
 *  - ESP32 Arduino core      (espressif/arduino-esp32)
 *  - WebServer               (included in ESP32 core)
 *  - Preferences             (included in ESP32 core)
 *  - WiFiClientSecure        (included in ESP32 core)
 *
 * Build: PlatformIO or Arduino IDE with ESP32 board package.
 * Board: esp32dev (or any ESP32 variant, 4MB flash recommended)
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ArduinoJson.h>

// ── NVS namespace + keys ────────────────────────────────────────────────────
static Preferences prefs;
static const char* PREF_NS  = "ac";

static const char* KEY_SSID = "ssid";
static const char* KEY_PASS = "pass";
static const char* KEY_LURL = "lurl";
static const char* KEY_LKEY = "lkey";
static const char* KEY_LMOD = "lmod";

// ── WiFi state ────────────────────────────────────────────────────────────
static char   wifiSSID[64]  = "";
static char   wifiPass[64]  = "";
static bool   wifiConfigured = false;
static bool   wifiNeedsRetry = false;

// ── LLM state ─────────────────────────────────────────────────────────────
static char   llmUrl[256]   = "https://ai.sumopod.com/v1/chat/completions";
static char   llmKey[128]   = "";
static char   llmModel[64]  = "gpt-4o-mini";

// ── Shared JSON documents (avoid repeated allocation) ────────────────────
static JsonDocument sendDoc;
static JsonDocument recvDoc;
static JsonDocument apiDoc;

// ── Web server ────────────────────────────────────────────────────────────
static WebServer server(80);
static bool      serverStarted = false;

// ─────────────────────────────────────────────────────────────────────────
// JSON serial helpers
// ─────────────────────────────────────────────────────────────────────────

/** Send { "ok": <ok>, "msg": "<msg>" } over Serial */
static void jsonResult(bool ok, const char* msg) {
  sendDoc.clear();
  sendDoc["ok"]  = ok;
  sendDoc["msg"] = msg;
  serializeJson(sendDoc, Serial);
  Serial.println();
}

/** Send full WiFi + system status */
static void jsonStatus() {
  sendDoc.clear();
  sendDoc["ok"]        = true;
  sendDoc["ssid"]      = wifiConfigured ? wifiSSID : "";
  sendDoc["connected"] = (WiFi.status() == WL_CONNECTED);
  if (WiFi.status() == WL_CONNECTED) {
    sendDoc["ip"] = WiFi.localIP().toString();
  }
  sendDoc["uptime"] = millis() / 1000;
  sendDoc["heap"]   = ESP.getFreeHeap();
  serializeJson(sendDoc, Serial);
  Serial.println();
}

/** Send current LLM config (key hidden) */
static void jsonLlmStatus() {
  sendDoc.clear();
  sendDoc["ok"]      = true;
  sendDoc["url"]     = llmUrl;
  sendDoc["model"]   = llmModel;
  sendDoc["has_key"] = (strlen(llmKey) > 0);
  serializeJson(sendDoc, Serial);
  Serial.println();
}

// ─────────────────────────────────────────────────────────────────────────
// NVS helpers
// ─────────────────────────────────────────────────────────────────────────

/**
 * Write WiFi credentials to NVS. Returns false and reports error on failure.
 * Performs write + readback verify.
 */
static bool saveWifiConfig() {
  if (!prefs.begin(PREF_NS, false)) {
    jsonResult(false, "nvs_write_open_fail");
    return false;
  }

  size_t w1 = prefs.putString(KEY_SSID, wifiSSID);
  size_t w2 = prefs.putString(KEY_PASS, wifiPass);
  prefs.end();

  if (w1 == 0 || w2 == 0) {
    jsonResult(false, "nvs_write_fail");
    return false;
  }

  // Readback verify
  if (!prefs.begin(PREF_NS, true)) {
    jsonResult(false, "nvs_verify_open_fail");
    return false;
  }
  String vs = prefs.getString(KEY_SSID, "");
  prefs.end();

  if (vs != String(wifiSSID)) {
    jsonResult(false, "nvs_verify_mismatch");
    return false;
  }

  return true;
}

/** Read WiFi credentials from NVS on boot */
static void loadWifiConfig() {
  wifiConfigured = false;
  wifiSSID[0]    = '\0';
  wifiPass[0]    = '\0';

  prefs.begin(PREF_NS, true);
  String ssid = prefs.getString(KEY_SSID, "");
  String pass = prefs.getString(KEY_PASS, "");
  prefs.end();

  if (ssid.length() > 0) {
    ssid.toCharArray(wifiSSID, sizeof(wifiSSID));
    pass.toCharArray(wifiPass, sizeof(wifiPass));
    wifiConfigured = true;
  }
}

/** Erase WiFi credentials from NVS */
static void clearWifiConfig() {
  if (prefs.begin(PREF_NS, false)) {
    prefs.remove(KEY_SSID);
    prefs.remove(KEY_PASS);
    prefs.end();
  }
  wifiConfigured = false;
  wifiNeedsRetry = false;
  wifiSSID[0]    = '\0';
  wifiPass[0]    = '\0';
  jsonResult(true, "config_cleared");
}

/** Read LLM config from NVS */
static void loadLlmConfig() {
  prefs.begin(PREF_NS, false);
  String url = prefs.getString(KEY_LURL, "");
  String key = prefs.getString(KEY_LKEY, "");
  String mod = prefs.getString(KEY_LMOD, "");
  prefs.end();

  if (url.length() > 0) url.toCharArray(llmUrl,   sizeof(llmUrl));
  if (key.length() > 0) key.toCharArray(llmKey,   sizeof(llmKey));
  if (mod.length() > 0) mod.toCharArray(llmModel, sizeof(llmModel));
}

/** Save LLM config to NVS */
static void saveLlmConfig() {
  prefs.begin(PREF_NS, false);
  prefs.putString(KEY_LURL, llmUrl);
  prefs.putString(KEY_LKEY, llmKey);
  prefs.putString(KEY_LMOD, llmModel);
  prefs.end();
}

// ─────────────────────────────────────────────────────────────────────────
// WiFi
// ─────────────────────────────────────────────────────────────────────────

/** Synchronous WiFi connect (called from WiFi task, not loop) */
static void doWifiConnect() {
  WiFi.disconnect(true);
  vTaskDelay(pdMS_TO_TICKS(500));
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSSID, wifiPass);

  bool ok = false;
  for (int i = 0; i < 40; i++) {          // 40 × 500 ms = 20 s timeout
    if (WiFi.status() == WL_CONNECTED) { ok = true; break; }
    vTaskDelay(pdMS_TO_TICKS(500));
  }

  if (ok) {
    jsonResult(true, "wifi_connected");
  } else {
    jsonResult(false, "wifi_failed");
  }
}

/** FreeRTOS task — handles connect + periodic reconnect */
static void wifiTask(void* param) {
  while (true) {
    if (wifiConfigured && (wifiNeedsRetry || WiFi.status() != WL_CONNECTED)) {
      wifiNeedsRetry = false;
      doWifiConnect();
    }
    vTaskDelay(pdMS_TO_TICKS(15000));
  }
}

// ─────────────────────────────────────────────────────────────────────────
// Web server / dashboard
// ─────────────────────────────────────────────────────────────────────────

static void handleRoot() {
  String html = F(R"(<!DOCTYPE html><html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ArduClaw</title>
<style>
body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;background:#f0f0f5;display:flex;flex-direction:column;align-items:center;justify-content:center;min-height:100vh;margin:0;padding:20px}
.card{background:#fff;border-radius:22px;padding:28px;max-width:420px;width:100%;box-shadow:0 2px 6px rgba(0,0,0,0.04),0 12px 36px rgba(0,0,0,0.07)}
h1{font-size:24px;font-weight:800;margin:0 0 4px;text-align:center}
h1 span{color:#FF6B00}
.sub{text-align:center;color:#6E6E73;font-size:13px;margin:0 0 20px}
.row{display:flex;align-items:center;justify-content:space-between;padding:10px 14px;background:#f8f8fb;border-radius:12px;margin-bottom:8px;font-size:14px}
.row .label{color:#6E6E73;font-size:12px;font-weight:700;text-transform:uppercase;letter-spacing:0.04em}
.row .val{font-weight:700;color:#1C1C1E}
.badge{display:inline-block;padding:3px 10px;border-radius:99px;font-size:12px;font-weight:700}
.ok{background:#e6f7e6;color:#2d8a2d}.warn{background:#fff3cd;color:#856404}
.footer{text-align:center;font-size:11px;color:#AEAEB2;margin-top:16px}
a.refresh{display:block;text-align:center;margin-top:12px;color:#FF6B00;font-size:13px;font-weight:700;text-decoration:none}
</style>
</head>
<body>
<div class="card">
<h1>Ardu<span>Claw</span></h1>
<p class="sub">AI Automation Runtime v0.6</p>
)");

  auto row = [&](const char* label, const String& val) {
    html += "<div class='row'><div class='label'>" + String(label) + "</div><div class='val'>" + val + "</div></div>\n";
  };

  bool connected = (WiFi.status() == WL_CONNECTED);
  row("Status",
      connected
        ? "<span class='badge ok'>Connected</span>"
        : "<span class='badge warn'>Disconnected</span>");
  row("SSID",     wifiConfigured ? String(wifiSSID) : "(none)");
  row("IP",       connected ? WiFi.localIP().toString() : "—");
  row("Uptime",   String(millis() / 1000) + " s");
  row("Free Heap", String(ESP.getFreeHeap()) + " B");
  row("LLM",      strlen(llmKey) > 0 ? "Configured" : "Not configured");

  html += F(R"(
<a class="refresh" href="/">↺ Refresh</a>
<div class="footer">ArduClaw v0.6 &middot; <a href="https://sumopod.com" style="color:#AEAEB2">sumopod.com</a></div>
</div></body></html>)");

  server.send(200, "text/html", html);
}

/** Start web server (idempotent) */
static void startWebServer() {
  if (serverStarted) return;
  server.on("/", handleRoot);
  server.begin();
  serverStarted = true;
}

// ─────────────────────────────────────────────────────────────────────────
// LLM
// ─────────────────────────────────────────────────────────────────────────

/**
 * Test LLM config: ensures WiFi + API key are set, starts web server,
 * then reports OK with IP so the flasher UI shows the success card.
 */
static void llmTest() {
  if (WiFi.status() != WL_CONNECTED) { jsonResult(false, "wifi_not_connected"); return; }
  if (strlen(llmKey) == 0)           { jsonResult(false, "api_key_missing");    return; }

  startWebServer();

  sendDoc.clear();
  sendDoc["ok"]  = true;
  sendDoc["msg"] = "llm_ok";
  sendDoc["ip"]  = WiFi.localIP().toString();
  serializeJson(sendDoc, Serial);
  Serial.println();
}

/**
 * Send a prompt to the configured LLM API and reply over Serial.
 * Uses raw HTTPS with WiFiClientSecure (cert verification disabled).
 */
static void llmChat(const char* prompt) {
  if (!prompt || strlen(prompt) == 0)    { jsonResult(false, "prompt_empty");      return; }
  if (WiFi.status() != WL_CONNECTED)    { jsonResult(false, "wifi_not_connected"); return; }
  if (strlen(llmKey) == 0)              { jsonResult(false, "api_key_missing");    return; }

  jsonResult(true, "llm_wait");   // immediate ack

  // ── Parse URL → host : port / path ──────────────────────────────────
  char host[128] = "";
  char path[256] = "";
  int  port      = 443;

  const char* u = llmUrl;
  if (strncmp(u, "https://", 8) == 0)      { u += 8; port = 443; }
  else if (strncmp(u, "http://", 7) == 0)  { u += 7; port = 80;  }

  const char* slash = strchr(u, '/');
  if (slash) {
    size_t hlen = (size_t)(slash - u);
    if (hlen >= sizeof(host)) hlen = sizeof(host) - 1;
    strncpy(host, u, hlen);
    host[hlen] = '\0';
    strncpy(path, slash, sizeof(path) - 1);
  } else {
    strncpy(host, u, sizeof(host) - 1);
    strncpy(path, "/v1/chat/completions", sizeof(path) - 1);
  }

  // Strip port from host if present
  char* colon = strchr(host, ':');
  if (colon) {
    port = atoi(colon + 1);
    *colon = '\0';
  }

  // ── Build JSON request body ──────────────────────────────────────────
  apiDoc.clear();
  apiDoc["model"] = llmModel;
  JsonArray msgs = apiDoc["messages"].to<JsonArray>();
  JsonObject msg = msgs.add<JsonObject>();
  msg["role"]    = "user";
  msg["content"] = prompt;
  apiDoc["max_tokens"]  = 512;
  apiDoc["temperature"] = 0.7;

  String reqBody;
  serializeJson(apiDoc, reqBody);

  // ── HTTPS request ────────────────────────────────────────────────────
  WiFiClientSecure client;
  client.setInsecure();   // skip certificate validation (fine for custom endpoints)

  if (!client.connect(host, port, /*timeout ms=*/8000)) {
    jsonResult(false, "llm_connect_fail");
    return;
  }

  // Send HTTP/1.1 request
  client.print("POST ");   client.print(path);   client.println(" HTTP/1.1");
  client.print("Host: ");  client.println(host);
  client.println("Content-Type: application/json");
  client.print("Authorization: Bearer "); client.println(llmKey);
  client.print("Content-Length: "); client.println(reqBody.length());
  client.println("Connection: close");
  client.println();
  client.print(reqBody);

  // ── Read response (with timeout) ─────────────────────────────────────
  String raw;
  raw.reserve(2048);
  unsigned long t0 = millis();
  while (millis() - t0 < 15000) {
    if (client.available()) {
      raw += (char)client.read();
    } else {
      if (raw.length() > 0 && !client.connected()) break;
      delay(5);
    }
  }
  client.stop();

  // ── Find JSON body (after \r\n\r\n) ─────────────────────────────────
  int bodyStart = raw.indexOf("\r\n\r\n");
  if (bodyStart < 0) { jsonResult(false, "llm_bad_response"); return; }
  String body = raw.substring(bodyStart + 4);

  // Handle chunked transfer encoding (strip chunk size lines)
  // Simple heuristic: if body starts with a hex digit, skip first line
  if (body.length() > 0 && isxdigit((unsigned char)body[0])) {
    int nl = body.indexOf('\n');
    if (nl >= 0) body = body.substring(nl + 1);
  }

  // ── Parse response JSON ──────────────────────────────────────────────
  apiDoc.clear();
  DeserializationError err = deserializeJson(apiDoc, body);
  if (err) {
    sendDoc.clear();
    sendDoc["ok"]  = false;
    sendDoc["msg"] = "llm_parse_fail";
    sendDoc["raw"] = body.substring(0, 200);
    serializeJson(sendDoc, Serial);
    Serial.println();
    return;
  }

  const char* content = apiDoc["choices"][0]["message"]["content"];
  if (!content) {
    const char* errMsg = apiDoc["error"]["message"];
    jsonResult(false, errMsg ? errMsg : "llm_no_response");
    return;
  }

  sendDoc.clear();
  sendDoc["ok"]    = true;
  sendDoc["msg"]   = "llm_ok";
  sendDoc["reply"] = content;
  serializeJson(sendDoc, Serial);
  Serial.println();
}

// ─────────────────────────────────────────────────────────────────────────
// Serial JSON command handler
// ─────────────────────────────────────────────────────────────────────────

static void handleSerial() {
  static String buf;

  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;   // ignore CR

    if (c == '\n') {
      buf.trim();

      // Empty line → handshake ping
      if (buf.length() == 0) {
        buf = "";
        jsonResult(true, "ready");
        continue;
      }

      // Parse JSON command
      recvDoc.clear();
      DeserializationError err = deserializeJson(recvDoc, buf);
      if (err) {
        jsonResult(false, "bad_json");
        buf = "";
        continue;
      }

      const char* cmd = recvDoc["c"] | "";

      // ── ws: WiFi set + connect ──────────────────────────────────────
      if (strcmp(cmd, "ws") == 0) {
        const char* s = recvDoc["s"] | "";
        const char* p = recvDoc["p"] | "";
        if (strlen(s) == 0) {
          jsonResult(false, "missing_ssid");
        } else {
          strncpy(wifiSSID, s, sizeof(wifiSSID) - 1);  wifiSSID[sizeof(wifiSSID)-1] = '\0';
          strncpy(wifiPass, p, sizeof(wifiPass) - 1);  wifiPass[sizeof(wifiPass)-1] = '\0';
          if (saveWifiConfig()) {
            wifiConfigured = true;
            wifiNeedsRetry = true;
            jsonResult(true, "wifi_saved_connecting");
          }
        }
      }
      // ── wst / st: status ────────────────────────────────────────────
      else if (strcmp(cmd, "wst") == 0 || strcmp(cmd, "st") == 0) {
        jsonStatus();
      }
      // ── wr: WiFi reset ──────────────────────────────────────────────
      else if (strcmp(cmd, "wr") == 0) {
        clearWifiConfig();
        WiFi.disconnect(true);
      }
      // ── rst: restart ────────────────────────────────────────────────
      else if (strcmp(cmd, "rst") == 0) {
        jsonResult(true, "restarting");
        Serial.flush();
        delay(200);
        ESP.restart();
      }
      // ── llm_set: save LLM config ────────────────────────────────────
      else if (strcmp(cmd, "llm_set") == 0) {
        const char* url = recvDoc["url"] | "";
        const char* key = recvDoc["key"] | "";
        const char* mod = recvDoc["model"] | "";
        if (strlen(url) > 0) strncpy(llmUrl,   url, sizeof(llmUrl)   - 1);
        if (strlen(key) > 0) strncpy(llmKey,   key, sizeof(llmKey)   - 1);
        if (strlen(mod) > 0) strncpy(llmModel, mod, sizeof(llmModel) - 1);
        saveLlmConfig();
        jsonResult(true, "llm_saved");
      }
      // ── llm_get: read LLM config ────────────────────────────────────
      else if (strcmp(cmd, "llm_get") == 0) {
        jsonLlmStatus();
      }
      // ── llm_test: verify config + start server ──────────────────────
      else if (strcmp(cmd, "llm_test") == 0) {
        llmTest();
      }
      // ── llm_chat: send prompt ────────────────────────────────────────
      else if (strcmp(cmd, "llm_chat") == 0) {
        const char* prompt = recvDoc["prompt"] | "";
        llmChat(prompt);
      }
      // ── start: start web server ──────────────────────────────────────
      else if (strcmp(cmd, "start") == 0) {
        if (WiFi.status() != WL_CONNECTED) {
          jsonResult(false, "wifi_not_connected");
        } else {
          startWebServer();
          sendDoc.clear();
          sendDoc["ok"]  = true;
          sendDoc["msg"] = "started";
          sendDoc["ip"]  = WiFi.localIP().toString();
          serializeJson(sendDoc, Serial);
          Serial.println();
        }
      }
      // ── unknown ──────────────────────────────────────────────────────
      else {
        jsonResult(false, "unknown_cmd");
      }

      buf = "";
    } else {
      // Buffer limit — prevent runaway memory use
      if (buf.length() < 512) buf += c;
    }
  }
}

// ─────────────────────────────────────────────────────────────────────────
// Setup / loop
// ─────────────────────────────────────────────────────────────────────────

void setup() {
  Serial.begin(115200);
  delay(800);   // let USB-UART settle

  Serial.println();
  Serial.println(F("========================="));
  Serial.println(F("  ArduClaw v0.6 booting  "));
  Serial.println(F("========================="));

  // Load configs from NVS
  loadWifiConfig();
  loadLlmConfig();

  // Report boot state
  if (wifiConfigured) {
    wifiNeedsRetry = true;
    jsonResult(true, "config_loaded");
  } else {
    jsonResult(true, "ready");
  }

  // WiFi task on core 1 (loop runs on core 1 by default, so pin wifi to same)
  xTaskCreatePinnedToCore(
    wifiTask,
    "wifi_task",
    4096,     // stack size in bytes
    NULL,     // parameter
    1,        // priority
    NULL,     // task handle
    1         // core 1
  );
}

void loop() {
  handleSerial();
  server.handleClient();
  vTaskDelay(pdMS_TO_TICKS(10));   // yield to RTOS scheduler
}