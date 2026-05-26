/**
 * ArduClaw v0.7 — Multi-Task Architecture
 *
 * Task Layout:
 *  ┌─────────────────────────────────────────────────────────┐
 *  │ Core 0 (Protocol CPU)                                   │
 *  │   loop_task  — serial handler + web server (16KB stack) │
 *  │   skill_task — queue-based skill executor  ( 6KB stack) │
 *  │   mqtt_task  — MQTT reconnect + loop       ( 6KB stack) │
 *  ├─────────────────────────────────────────────────────────┤
 *  │ Core 1 (App CPU)                                        │
 *  │   wifi_task  — wifi reconnect loop         ( 8KB stack) │
 *  │   llm_task   — per-request LLM call        (20KB stack) │
 *  └─────────────────────────────────────────────────────────┘
 *
 * Komunikasi antar task pakai:
 *  - QueueHandle_t  skillQueue   : main → skill_task
 *  - QueueHandle_t  skillResult  : skill_task → main
 *  - SemaphoreHandle_t llmDone   : llm_task → handleApiChat
 *  - SemaphoreHandle_t wifiReady : wifi_task → siapapun yang tunggu
 *
 * Serial Commands (JSON, newline-terminated):
 *  { "c": "ws",       "s": "SSID", "p": "password" }
 *  { "c": "wst" }  { "c": "wr" }  { "c": "rst" }  { "c": "st" }
 *  { "c": "llm_set", "url":"...", "key":"...", "model":"..." }
 *  { "c": "llm_get" }  { "c": "llm_test" }
 *  { "c": "llm_chat", "prompt":"..." }
 *  { "c": "start" }
 */

// ── Loop task stack — HARUS sebelum Arduino.h di-include ─────────────────
// Tanpa ini loop() jalan di stack default 8KB → crash saat handle LLM
#ifndef ARDUINO_LOOP_STACK_SIZE
  #define ARDUINO_LOOP_STACK_SIZE (16 * 1024)
#endif

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiClient.h>
#include <PubSubClient.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include "hal.h"
#include "skill.h"
#include "dashboard.h"

// ─────────────────────────────────────────────────────────────────────────
// NVS
// ─────────────────────────────────────────────────────────────────────────

static Preferences prefs;
static const char* PREF_NS  = "ac";
static const char* KEY_SSID = "ssid";
static const char* KEY_PASS = "pass";
static const char* KEY_LURL = "lurl";
static const char* KEY_LKEY = "lkey";
static const char* KEY_LMOD = "lmod";
static const char* KEY_MQH = "mqh";
static const char* KEY_MQP = "mqp";
static const char* KEY_MQU = "mqu";
static const char* KEY_MQPS = "mqps";
static const char* KEY_MQC = "mqc";

// ─────────────────────────────────────────────────────────────────────────
// WiFi state
// ─────────────────────────────────────────────────────────────────────────

static char wifiSSID[64]    = "";
static char wifiPass[64]    = "";
static bool wifiConfigured  = false;
static bool wifiNeedsRetry  = false;

// ─────────────────────────────────────────────────────────────────────────
// LLM state
// ─────────────────────────────────────────────────────────────────────────

static char llmUrl[256]  = "https://ai.sumopod.com/v1/chat/completions";
static char llmKey[128]  = "";
static char llmModel[64] = "gpt-4o-mini";

// ─────────────────────────────────────────────────────────────────────────
// MQTT state
// ─────────────────────────────────────────────────────────────────────────

static char     mqttHost[64]     = "";
static uint16_t mqttPort         = 1883;
static char     mqttUser[32]     = "";
static char     mqttPass[32]     = "";
static char     mqttClientId[32] = "arduclaw";
static bool     mqttConfigured   = false;
static bool     mqttNeedsRetry   = false;

// ─────────────────────────────────────────────────────────────────────────
// Shared JSON docs (hanya dipakai dari loop_task/serial handler)
// ─────────────────────────────────────────────────────────────────────────

static JsonDocument sendDoc;
static JsonDocument recvDoc;

// ─────────────────────────────────────────────────────────────────────────
// Web server
// ─────────────────────────────────────────────────────────────────────────

static WebServer server(80);
static bool      serverStarted = false;

// ─────────────────────────────────────────────────────────────────────────
// SkillQueue — kirim request dari loop_task ke skill_task
// ─────────────────────────────────────────────────────────────────────────

#define SKILL_TOOL_LEN  32
#define SKILL_ARGS_LEN 128

struct SkillRequest {
  char tool[SKILL_TOOL_LEN];
  char argsJson[SKILL_ARGS_LEN];  // JSON string args, di-parse di skill_task
};

using skill::SkillResponse;

static QueueHandle_t skillQueue    = nullptr;  // SkillRequest
static QueueHandle_t skillRespQ    = nullptr;  // SkillResponse

// ─────────────────────────────────────────────────────────────────────────
// LLM Task state
// ─────────────────────────────────────────────────────────────────────────

struct LLMResult {
  bool   success;
  char   response[512];
  int    skillsExecuted;
  char   skillResultsJson[512];
};

struct LLMTaskParams {
  char      prompt[256];
  bool      done;
  LLMResult result;
};

static LLMTaskParams     llmTaskParams;
static SemaphoreHandle_t llmDone     = nullptr;
static SemaphoreHandle_t wifiReady   = nullptr;

// ─────────────────────────────────────────────────────────────────────────
// Strapping pin check
// ─────────────────────────────────────────────────────────────────────────

static const uint8_t STRAPPING_PINS[]     = {0, 2, 5, 12, 15};
static const uint8_t STRAPPING_PIN_COUNT  = 5;

static bool isStrappingPin(uint8_t pin) {
  for (uint8_t i = 0; i < STRAPPING_PIN_COUNT; i++)
    if (STRAPPING_PINS[i] == pin) return true;
  return false;
}

// ─────────────────────────────────────────────────────────────────────────
// Serial JSON helpers (dipanggil hanya dari loop_task)
// ─────────────────────────────────────────────────────────────────────────

static void jsonResult(bool ok, const char* msg) {
  sendDoc.clear();
  sendDoc["ok"]  = ok;
  sendDoc["msg"] = msg;
  serializeJson(sendDoc, Serial);
  Serial.println();
}

static void jsonStatus() {
  sendDoc.clear();
  sendDoc["ok"]             = true;
  sendDoc["ssid"]           = wifiConfigured ? wifiSSID : "";
  sendDoc["connected"]      = (WiFi.status() == WL_CONNECTED);
  if (WiFi.status() == WL_CONNECTED) sendDoc["ip"] = WiFi.localIP().toString();
  sendDoc["uptime"]         = millis() / 1000;
  sendDoc["heap"]           = ESP.getFreeHeap();
  sendDoc["min_heap"]       = ESP.getMinFreeHeap();
  sendDoc["mqtt_configured"] = mqttConfigured;
  if (mqttConfigured) {
    sendDoc["mqtt_host"] = mqttHost;
    sendDoc["mqtt_port"] = mqttPort;
  }
  serializeJson(sendDoc, Serial);
  Serial.println();
}

static void jsonLlmStatus() {
  sendDoc.clear();
  sendDoc["ok"]      = true;
  sendDoc["url"]     = llmUrl;
  sendDoc["model"]   = llmModel;
  sendDoc["has_key"] = (strlen(llmKey) > 0);
  serializeJson(sendDoc, Serial);
  Serial.println();
}

static void jsonMqttStatus() {
  sendDoc.clear();
  sendDoc["ok"]            = true;
  sendDoc["host"]          = mqttHost;
  sendDoc["port"]          = mqttPort;
  sendDoc["user"]          = mqttUser;
  sendDoc["client_id"]     = mqttClientId;
  sendDoc["configured"]    = mqttConfigured;
  sendDoc["subs"]          = skill::mqttActiveSubCount();
  sendDoc["sub_details"]   = skill::listMqttSubs();
  serializeJson(sendDoc, Serial);
  Serial.println();
}

// ─────────────────────────────────────────────────────────────────────────
// NVS helpers
// ─────────────────────────────────────────────────────────────────────────

static bool saveWifiConfig() {
  if (!prefs.begin(PREF_NS, false)) { jsonResult(false, "nvs_write_open_fail"); return false; }
  size_t w1 = prefs.putString(KEY_SSID, wifiSSID);
  size_t w2 = prefs.putString(KEY_PASS, wifiPass);
  prefs.end();
  if (w1 == 0 || w2 == 0) { jsonResult(false, "nvs_write_fail"); return false; }
  if (!prefs.begin(PREF_NS, true)) { jsonResult(false, "nvs_verify_open_fail"); return false; }
  String vs = prefs.getString(KEY_SSID, "");
  prefs.end();
  if (vs != String(wifiSSID)) { jsonResult(false, "nvs_verify_mismatch"); return false; }
  return true;
}

static void loadWifiConfig() {
  wifiConfigured = false;
  wifiSSID[0] = '\0'; wifiPass[0] = '\0';
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

static void clearWifiConfig() {
  if (prefs.begin(PREF_NS, false)) {
    prefs.remove(KEY_SSID); prefs.remove(KEY_PASS);
    prefs.end();
  }
  wifiConfigured = false; wifiNeedsRetry = false;
  wifiSSID[0] = '\0'; wifiPass[0] = '\0';
  jsonResult(true, "config_cleared");
}

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

static void saveLlmConfig() {
  prefs.begin(PREF_NS, false);
  prefs.putString(KEY_LURL, llmUrl);
  prefs.putString(KEY_LKEY, llmKey);
  prefs.putString(KEY_LMOD, llmModel);
  prefs.end();
}

// ─────────────────────────────────────────────────────────────────────────
// MQTT NVS helpers
// ─────────────────────────────────────────────────────────────────────────

static void saveMqttConfig() {
  prefs.begin(PREF_NS, false);
  prefs.putString(KEY_MQH, mqttHost);
  prefs.putString(KEY_MQP, String(mqttPort));
  prefs.putString(KEY_MQU, mqttUser);
  prefs.putString(KEY_MQPS, mqttPass);
  prefs.putString(KEY_MQC, mqttClientId);
  prefs.end();
}

static void loadMqttConfig() {
  mqttConfigured = false;
  mqttHost[0] = '\0'; mqttUser[0] = '\0'; mqttPass[0] = '\0'; mqttClientId[0] = '\0';
  mqttPort = 1883;
  prefs.begin(PREF_NS, true);
  String host = prefs.getString(KEY_MQH, "");
  String port = prefs.getString(KEY_MQP, "1883");
  String user = prefs.getString(KEY_MQU, "");
  String pass = prefs.getString(KEY_MQPS, "");
  String cid  = prefs.getString(KEY_MQC, "arduclaw");
  prefs.end();
  if (host.length() > 0) {
    host.toCharArray(mqttHost, sizeof(mqttHost));
    mqttPort = (uint16_t)port.toInt();
    user.toCharArray(mqttUser, sizeof(mqttUser));
    pass.toCharArray(mqttPass, sizeof(mqttPass));
    cid.toCharArray(mqttClientId, sizeof(mqttClientId));
    mqttConfigured = true;
  }
}

// ─────────────────────────────────────────────────────────────────────────
// TASK 1: wifi_task — Core 1, 8KB
// Tanggung jawab: connect, retry tiap 15 detik, beri sinyal wifiReady
// ─────────────────────────────────────────────────────────────────────────

static void startWebServer();  // forward decl

static void wifiTask(void* param) {
  static bool serverAutoStarted = false;

  while (true) {
    // Reconnect jika perlu
    if (wifiConfigured && (wifiNeedsRetry || WiFi.status() != WL_CONNECTED)) {
      wifiNeedsRetry = false;

      WiFi.disconnect(true);
      vTaskDelay(pdMS_TO_TICKS(500));
      WiFi.mode(WIFI_STA);
      WiFi.begin(wifiSSID, wifiPass);

      bool ok = false;
      for (int i = 0; i < 40 && !ok; i++) {
        if (WiFi.status() == WL_CONNECTED) ok = true;
        else vTaskDelay(pdMS_TO_TICKS(500));
      }

      if (ok) {
        Serial.printf("[WIFI] Connected: %s\n", WiFi.localIP().toString().c_str());
        jsonResult(true, "wifi_connected");
        xSemaphoreGive(wifiReady);  // sinyal ke siapapun yang tunggu WiFi
      } else {
        Serial.println(F("[WIFI] Connect failed, will retry"));
      }
    }

    // Auto-start web server sekali setelah WiFi + LLM key tersedia
    if (!serverAutoStarted && WiFi.status() == WL_CONNECTED && strlen(llmKey) > 0) {
      serverAutoStarted = true;
      vTaskDelay(pdMS_TO_TICKS(500));
      startWebServer();
      Serial.printf("[WIFI] Web server started: http://%s\n",
                    WiFi.localIP().toString().c_str());
    }

    // Monitor heap setiap siklus
    Serial.printf("[MEM]  free=%u  min_ever=%u\n",
                  ESP.getFreeHeap(), ESP.getMinFreeHeap());

    vTaskDelay(pdMS_TO_TICKS(15000));
  }
}

// ── MQTT reconnect callback — dipanggil dari skill_task ──
static void requestMqttReconnect() {
  mqttNeedsRetry = true;
}

// ─────────────────────────────────────────────────────────────────────────
// MQTT publish queue — aman dipanggil dari task lain (skill_task)
// ─────────────────────────────────────────────────────────────────────────

#define MQTT_PUB_QUEUE_SIZE 4

struct MqttPubRequest {
  char topic[64];
  char payload[256];
};

static QueueHandle_t mqttPubQueue = nullptr;

static bool queueMqttPublish(const char* topic, const char* payload) {
  if (!mqttPubQueue) return false;
  MqttPubRequest req;
  strncpy(req.topic, topic, sizeof(req.topic) - 1);
  req.topic[sizeof(req.topic) - 1] = '\0';
  strncpy(req.payload, payload, sizeof(req.payload) - 1);
  req.payload[sizeof(req.payload) - 1] = '\0';
  return xQueueSend(mqttPubQueue, &req, pdMS_TO_TICKS(100)) == pdTRUE;
}

// ─────────────────────────────────────────────────────────────────────────
// TASK 2.5: mqtt_task — Core 0, 6KB
// Tanggung jawab: konek ke MQTT broker, subscribe ulang, loop client,
//                 proses publish queue, dispatch incoming messages
// ─────────────────────────────────────────────────────────────────────────

static void mqttCallback(char* topic, byte* payload, unsigned int length) {
  char topicStr[64];
  strncpy(topicStr, topic, sizeof(topicStr) - 1);
  topicStr[sizeof(topicStr) - 1] = '\0';

  char payloadStr[256];
  unsigned int len = length < sizeof(payloadStr) - 1 ? length : sizeof(payloadStr) - 1;
  memcpy(payloadStr, payload, len);
  payloadStr[len] = '\0';

  skill::mqttOnMessage(topicStr, payloadStr);
}

// PubSubClient globals — local static, hanya diakses dari mqtt_task
static WiFiClient   mqttWifiClient;
static PubSubClient mqttPubClient(mqttWifiClient);

static void mqttTask(void* param) {
  (void)param;

  while (true) {
    if (!mqttConfigured || strlen(mqttHost) == 0) {
      vTaskDelay(pdMS_TO_TICKS(5000));
      continue;
    }

    // Tunggu WiFi sebelum connect MQTT
    if (WiFi.status() != WL_CONNECTED) {
      vTaskDelay(pdMS_TO_TICKS(3000));
      continue;
    }

    if (mqttNeedsRetry) {
      mqttNeedsRetry = false;
      if (mqttPubClient.connected()) mqttPubClient.disconnect();
    }

    if (!mqttPubClient.connected()) {
      mqttPubClient.setServer(mqttHost, mqttPort);
      mqttPubClient.setCallback(mqttCallback);

      Serial.printf("[MQTT] Connecting to %s:%u as %s\n",
                    mqttHost, mqttPort, mqttClientId);

      boolean ok = false;
      if (strlen(mqttUser) > 0)
        ok = mqttPubClient.connect(mqttClientId, mqttUser, mqttPass);
      else
        ok = mqttPubClient.connect(mqttClientId);

      if (ok) {
        Serial.printf("[MQTT] Connected to %s:%u\n", mqttHost, mqttPort);
        // Built-in MQTT → GPIO: hello/10219201/test on/off → pin 27
        if (mqttPubClient.subscribe("hello/10219201/test")) {
          Serial.printf("[MQTT] Subscribed to hello/10219201/test\n");
        } else {
          Serial.printf("[MQTT] WARN: subscribe hello/10219201/test failed\n");
        }
        skill::mqttMarkAllUnsynced();
        skill::mqttSyncSubs(
          +[](const char* t) -> bool { return mqttPubClient.subscribe(t); },
          +[](const char* t) -> bool { return mqttPubClient.unsubscribe(t); }
        );
      } else {
        Serial.printf("[MQTT] Connect failed rc=%d. Retry in 15s\n", mqttPubClient.state());
        vTaskDelay(pdMS_TO_TICKS(15000));
        continue;
      }
    }

    // Process publish queue
    MqttPubRequest pubReq;
    while (xQueueReceive(mqttPubQueue, &pubReq, 0) == pdTRUE) {
      if (mqttPubClient.connected()) {
        mqttPubClient.publish(pubReq.topic, pubReq.payload);
        Serial.printf("[MQTT] Published: %s\n", pubReq.topic);
      }
    }

    // Sync subscriptions (brokerSynced tracking)
    skill::mqttSyncSubs(
      +[](const char* t) -> bool { return mqttPubClient.subscribe(t); },
      +[](const char* t) -> bool { return mqttPubClient.unsubscribe(t); }
    );

    mqttPubClient.loop();
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// ─────────────────────────────────────────────────────────────────────────
// TASK 3: skill_task — Core 0, 6KB
// Tanggung jawab: terima SkillRequest dari queue, eksekusi, kirim response
// Dipisah ke Core 0 agar tidak bentrok dgn llm_task di Core 1
// ─────────────────────────────────────────────────────────────────────────

// Doc kecil khusus skill_task — tidak dipakai task lain
static JsonDocument skillArgDoc;

static void skillTask(void* param) {
  SkillRequest  req;
  SkillResponse resp;

  while (true) {
    // Block sampai ada request (portMAX_DELAY = tunggu selamanya)
    if (xQueueReceive(skillQueue, &req, portMAX_DELAY) == pdTRUE) {
      memset(&resp, 0, sizeof(resp));
      strncpy(resp.tool, req.tool, sizeof(resp.tool) - 1);

      Serial.printf("[SKILL_TASK] Executing: %s  args: %s\n",
                    req.tool, req.argsJson);

      // Cek strapping pin dari argsJson sebelum parse
      // (cepat, tanpa parse JSON)
      // Parse args
      skillArgDoc.clear();
      JsonObject args;

      if (strlen(req.argsJson) > 2) {
        DeserializationError err = deserializeJson(skillArgDoc, req.argsJson);
        if (err) {
          resp.success = false;
          snprintf(resp.message, sizeof(resp.message),
                   "Args JSON parse error: %s", err.c_str());
          xQueueSend(skillRespQ, &resp, pdMS_TO_TICKS(1000));
          continue;
        }
        args = skillArgDoc.as<JsonObject>();

        // Strapping pin warning
        if (args["pin"].is<int>()) {
          uint8_t pin = args["pin"].as<uint8_t>();
          if (isStrappingPin(pin)) {
            Serial.printf("[WARN] Strapping pin GPIO %d! Gunakan 13,14,16-19,21,25-27,32,33\n", pin);
          }
        }
      } else {
        // Skill tanpa args (misal system.status)
        skillArgDoc.clear();
        args = skillArgDoc.to<JsonObject>();
      }

      skill::SkillResult result = skill::executeSkillByName(String(req.tool), args);

      resp.success = result.success;
      strncpy(resp.message, result.message.c_str(), sizeof(resp.message) - 1);

      // Kirim response ke queue
      if (xQueueSend(skillRespQ, &resp, pdMS_TO_TICKS(2000)) != pdTRUE) {
        Serial.println(F("[SKILL_TASK] Response queue full!"));
      }
    }
  }
}

// ─────────────────────────────────────────────────────────────────────────
// Helper: kirim skill ke skill_task dan tunggu hasilnya
// Dipanggil dari llm_task (bukan loop_task), aman karena pakai queue
// ─────────────────────────────────────────────────────────────────────────

static bool dispatchSkill(const char* tool, const char* argsJson,
                           SkillResponse& outResp,
                           TickType_t timeout = pdMS_TO_TICKS(5000)) {
  SkillRequest req;
  memset(&req, 0, sizeof(req));
  strncpy(req.tool,     tool,     sizeof(req.tool)     - 1);
  strncpy(req.argsJson, argsJson, sizeof(req.argsJson) - 1);

  if (xQueueSend(skillQueue, &req, pdMS_TO_TICKS(1000)) != pdTRUE) {
    outResp.success = false;
    strncpy(outResp.message, "Skill queue full", sizeof(outResp.message) - 1);
    return false;
  }

  TickType_t remaining = timeout;
  while (remaining > 0) {
    TickType_t t0 = xTaskGetTickCount();
    if (xQueueReceive(skillRespQ, &outResp, remaining) != pdTRUE) {
      outResp.success = false;
      strncpy(outResp.message, "Skill timeout", sizeof(outResp.message) - 1);
      return false;
    }
    if (strcmp(outResp.tool, req.tool) == 0) break;
    TickType_t elapsed = xTaskGetTickCount() - t0;
    remaining = (elapsed >= remaining) ? 0 : remaining - elapsed;
  }

  return outResp.success;
}

// ─────────────────────────────────────────────────────────────────────────
// LLM Helpers
// ─────────────────────────────────────────────────────────────────────────

static String extractJsonFromResponse(const String& raw) {
  int start = raw.indexOf('{');
  if (start < 0) return "";
  int end = raw.lastIndexOf('}');
  if (end < start) return "";
  return raw.substring(start, end + 1);
}

// Doc khusus llm_task — tidak dipakai task lain
// HANYA satu doc, tidak ada doc kedua untuk content
static JsonDocument llmApiDoc;

// ─────────────────────────────────────────────────────────────────────────
// Struct untuk satu skill yang sudah di-extract dari LLM content.
// Disimpan sebagai plain char[] — tidak ada pointer ke JsonDocument
// sehingga aman di-pass antar scope / task.
// ─────────────────────────────────────────────────────────────────────────

struct ExtractedSkill {
  char tool[SKILL_TOOL_LEN];
  char argsJson[SKILL_ARGS_LEN];
};

static const uint8_t MAX_EXTRACTED_SKILLS = 8;

/**
 * Parse content LLM → response text + array of ExtractedSkill.
 *
 * TIDAK ada JsonArray/JsonObject yang di-return atau disimpan lintas
 * scope. Semua data di-copy ke plain char[] sebelum llmApiDoc.clear().
 *
 * @param content     String content dari LLM
 * @param outSkills   Array output ExtractedSkill (stack-allocated oleh caller)
 * @param outCount    Jumlah skill yang berhasil di-extract
 * @param outResponse Buffer untuk response text
 * @param respLen     Ukuran buffer outResponse
 */
static void parseLLMContent(const String& content,
                             ExtractedSkill* outSkills,
                             uint8_t&        outCount,
                             char*           outResponse,
                             size_t          respLen) {
  outCount = 0;
  strncpy(outResponse, content.c_str(), respLen - 1);
  outResponse[respLen - 1] = '\0';

  String toParse = content;
  toParse.trim();

  // Strip markdown fences
  if (toParse.startsWith("```json") || toParse.startsWith("```")) {
    int s = toParse.indexOf('\n');
    int e = toParse.lastIndexOf("```");
    if (s >= 0 && e > s) { toParse = toParse.substring(s + 1, e); toParse.trim(); }
  }

  if (toParse.length() == 0 || toParse[0] != '{') return;

  // Parse ke llmApiDoc — REUSE doc yang sudah clear sebelumnya
  // Caller HARUS sudah clear llmApiDoc sebelum panggil fungsi ini
  DeserializationError err = deserializeJson(llmApiDoc, toParse);
  if (err != DeserializationError::Ok) {
    Serial.printf("[LLM] Content parse error: %s\n", err.c_str());
    return;
  }

  // Salin response text ke buffer caller SEBELUM apapun yang bisa realloc doc
  const char* resp = llmApiDoc["response"] | "";
  if (strlen(resp) > 0) {
    strncpy(outResponse, resp, respLen - 1);
    outResponse[respLen - 1] = '\0';
  }

  // Extract skills — salin tool + args ke plain struct SEGERA
  if (!llmApiDoc["skills"].is<JsonArray>()) return;

  JsonArray skills = llmApiDoc["skills"].as<JsonArray>();
  for (JsonObject skillObj : skills) {
    if (outCount >= MAX_EXTRACTED_SKILLS) break;

    const char* toolName = skillObj["tool"] | "";
    if (strlen(toolName) == 0) continue;

    ExtractedSkill& es = outSkills[outCount];
    memset(&es, 0, sizeof(es));
    strncpy(es.tool, toolName, sizeof(es.tool) - 1);

    // Serialize args ke JSON string — data di-copy ke es.argsJson
    if (skillObj["args"].is<JsonObject>()) {
      serializeJson(skillObj["args"], es.argsJson, sizeof(es.argsJson));
    } else {
      strncpy(es.argsJson, "{}", sizeof(es.argsJson) - 1);
    }

    outCount++;
    Serial.printf("[LLM] Extracted skill[%d]: %s  args=%s\n",
                  outCount - 1, es.tool, es.argsJson);
  }

  // PENTING: setelah semua data di-copy ke plain struct,
  // baru boleh clear doc. Tidak ada pointer ke doc yang tersisa.
  llmApiDoc.clear();
}

// ─────────────────────────────────────────────────────────────────────────
// TASK 3: llm_task — Core 1, 20KB, spawn per-request
// Tanggung jawab: HTTP ke LLM API, parse response, dispatch skill via queue
// ─────────────────────────────────────────────────────────────────────────

static void llmCallTask(void* param) {
  LLMTaskParams* p = (LLMTaskParams*)param;
  LLMResult&     r = p->result;
  memset(&r, 0, sizeof(r));
  r.skillsExecuted    = 0;
  strncpy(r.skillResultsJson, "[]", sizeof(r.skillResultsJson) - 1);

  // ── Guard ──────────────────────────────────────────────────────────────
  if (strlen(llmUrl) == 0 || strlen(llmKey) == 0) {
    strncpy(r.response, "LLM not configured", sizeof(r.response) - 1);
    r.success = false;
    goto done;
  }
  if (WiFi.status() != WL_CONNECTED) {
    strncpy(r.response, "WiFi not connected", sizeof(r.response) - 1);
    r.success = false;
    goto done;
  }
  if (ESP.getFreeHeap() < 10000) {
    snprintf(r.response, sizeof(r.response),
             "Heap terlalu kecil: %u B", ESP.getFreeHeap());
    r.success = false;
    goto done;
  }

  {
    // ── System prompt ─────────────────────────────────────────────────────
    static const char SYSTEM_PROMPT[] PROGMEM =
      "Kamu adalah ArduClaw, AI automation runtime untuk ESP32. "
      "Saat diminta kontrol hardware, SELALU balas HANYA dengan JSON valid:\n"
      "{\"response\": \"penjelasan singkat\", \"skills\": [{\"tool\": \"nama\", \"args\": {...}}]}\n"
      "\nDaftar skills yang tersedia:\n"
      "  gpio.write       {pin:int, value:bool}                 — tulis HIGH/LOW\n"
      "  gpio.read        {pin:int}                             — baca nilai pin\n"
      "  gpio.mode        {pin:int, mode:string}                — set mode pin (output/input/input_pullup)\n"
      "  gpio.blink       {pin:int, on_ms:int, off_ms:int, count:int} — kedip N kali lalu berhenti\n"
      "  gpio.blink_start {pin:int, on_ms:int, off_ms:int}     — mulai kedip terus-menerus (non-blocking)\n"
      "  gpio.blink_stop  {pin:int}                            — hentikan kedip (tanpa pin = stop semua)\n"
      "  gpio.monitor     {pin:int, trigger:string, action:string, action_args:obj, poll_ms:int, debounce_ms:int, once:bool} — pantau pin, jalankan action saat trigger terpenuhi\n"
      "  gpio.monitor_stop {pin:int}                            — hentikan monitor (tanpa pin = stop semua)\n"
      "  adc.read              {pin:int}                             — baca ADC (gunakan pin 32-39)\n"
      "  system.status         {}                                    — status sistem\n"
      "  system.clear_persist  {}                                    — hapus logika. Reboot → tidak ada logika sama sekali\n"
      "  mqtt.publish       {topic:string, payload:string}           — kirim pesan ke MQTT broker\n"
      "  mqtt.subscribe     {topic:string, action:string, action_args:obj} — subscribe topic, jalankan action saat pesan masuk. Gunakan {PAYLOAD} di action_args untuk menyisipkan isi pesan MQTT, contoh: {\"pin\":27,\"value\":\"{PAYLOAD}\"}\n"
      "  mqtt.unsubscribe   {topic:string}                           — unsubscribe topic (kosongkan = hapus semua)\n"
      "  system.reset_factory  {}                                    — factory reset. Reboot → benar-benar kosong, tidak ada logika\n"
      "\nAturan pemilihan skill blink:\n"
      "  - 'kedip X kali' → gpio.blink dengan count=X\n"
      "  - 'kedip terus' / 'blink loop' / tanpa jumlah → gpio.blink_start\n"
      "  - 'stop kedip' / 'matikan kedip' → gpio.blink_stop\n"
      "  - on_ms dan off_ms default 500 jika tidak disebutkan\n"
      "\nAturan gpio.monitor:\n"
      "  trigger: 'low' (HIGH→LOW, button ditekan pull-up), 'high' (LOW→HIGH, button dilepas), 'change' (setiap perubahan)\n"
      "  action: nama skill yang akan dijalankan saat trigger, action_args: argumen untuk skill tsb\n"
      "  Contoh: button pull-up pin 26 → LED blink pin 27:\n"
      "    {\"tool\":\"gpio.monitor\",\"args\":{\"pin\":26,\"trigger\":\"low\",\"action\":\"gpio.blink_start\",\"action_args\":{\"pin\":27,\"on_ms\":500,\"off_ms\":500}}}\n"
      "    {\"tool\":\"gpio.monitor\",\"args\":{\"pin\":26,\"trigger\":\"high\",\"action\":\"gpio.blink_stop\",\"action_args\":{\"pin\":27}}}\n"
      "\nContoh perintah user dan respons yang benar:\n"
      "  - 'matikan semua logika' → gpio.monitor_stop {} lalu system.clear_persist {}. Reboot → semua mati total\n"
      "  - 'reset ke factory default' → gpio.monitor_stop {} lalu system.reset_factory {}. Reboot → kosong total\n"
      "  - 'LED pin 27 nyala' → gpio.write {pin:27, value:true}\n"
      "  - 'baca suhu' → adc.read {pin:34}\n"
      "  - 'mqtt subscribe hello/test → gpio.write pin 27 on/off' → mqtt.subscribe {topic:'hello/test', action:'gpio.write', action_args:{pin:27, value:'{PAYLOAD}'}}\n"
      "  - 'terima data mqtt topik X on/off ke pin Y' → mqtt.subscribe {topic:'X', action:'gpio.write', action_args:{pin:Y, value:'{PAYLOAD}'}}\n"
      "\nPin 0,2,5,12,15 adalah strapping pin (hanya peringatan, tetap bisa dipakai).\n"
      "Balas HANYA JSON valid, tanpa markdown, tanpa teks di luar JSON.";

    // ── Build request body ────────────────────────────────────────────────
    llmApiDoc.clear();
    llmApiDoc["model"]       = llmModel;
    llmApiDoc["temperature"] = 0.3;
    llmApiDoc["max_tokens"]  = 512;
    JsonArray msgs = llmApiDoc["messages"].to<JsonArray>();
    {
      JsonObject sys    = msgs.add<JsonObject>();
      sys["role"]       = "system";
      sys["content"]    = (const char*)SYSTEM_PROMPT;
      JsonObject user   = msgs.add<JsonObject>();
      user["role"]      = "user";
      user["content"]   = p->prompt;
    }

    String reqBody;
    reqBody.reserve(800);
    serializeJson(llmApiDoc, reqBody);
    llmApiDoc.clear();  // bebaskan RAM sebelum buka socket

    // ── Parse URL ─────────────────────────────────────────────────────────
    char host[128] = "";
    char path[256] = "/v1/chat/completions";
    int  port      = 443;

    const char* u = llmUrl;
    if      (strncmp(u, "https://", 8) == 0) { u += 8; port = 443; }
    else if (strncmp(u, "http://",  7) == 0) { u += 7; port = 80;  }

    const char* sl = strchr(u, '/');
    if (sl) {
      size_t hlen = (size_t)(sl - u);
      if (hlen >= sizeof(host)) hlen = sizeof(host) - 1;
      strncpy(host, u, hlen); host[hlen] = '\0';
      strncpy(path, sl, sizeof(path) - 1);
    } else {
      strncpy(host, u, sizeof(host) - 1);
    }
    char* colon = strchr(host, ':');
    if (colon) { port = atoi(colon + 1); *colon = '\0'; }

    // ── HTTPS connect ─────────────────────────────────────────────────────
    WiFiClientSecure client;
    client.setInsecure();

    if (!client.connect(host, port, 10000)) {
      strncpy(r.response, "Gagal konek ke LLM server", sizeof(r.response) - 1);
      r.success = false;
      goto done;
    }

    client.printf("POST %s HTTP/1.1\r\n", path);
    client.printf("Host: %s\r\n", host);
    client.print("Content-Type: application/json\r\n");
    client.printf("Authorization: Bearer %s\r\n", llmKey);
    client.printf("Content-Length: %u\r\n", reqBody.length());
    client.print("Accept-Encoding: identity\r\n");
    client.print("Connection: close\r\n\r\n");
    client.print(reqBody);
    reqBody = "";  // bebaskan RAM

    // ── Baca response ─────────────────────────────────────────────────────
    String raw;
    raw.reserve(2048);
    unsigned long t0 = millis();
    while (millis() - t0 < 20000) {
      while (client.available()) { raw += (char)client.read(); t0 = millis(); }
      if (!client.connected() && !client.available()) break;
      vTaskDelay(pdMS_TO_TICKS(5));
    }
    client.stop();

    Serial.printf("[LLM] Raw len=%u  free_heap=%u\n",
                  raw.length(), ESP.getFreeHeap());

    // ── Parse API JSON ────────────────────────────────────────────────────
    String body = extractJsonFromResponse(raw);
    raw = "";  // bebaskan RAM

    if (body.length() == 0) {
      strncpy(r.response, "Tidak ada JSON dalam response", sizeof(r.response) - 1);
      r.success = false;
      goto done;
    }

    llmApiDoc.clear();
    if (deserializeJson(llmApiDoc, body) != DeserializationError::Ok) {
      strncpy(r.response, "Parse API response gagal", sizeof(r.response) - 1);
      r.success = false;
      goto done;
    }
    body = "";

    if (llmApiDoc["error"].is<JsonObject>()) {
      const char* em = llmApiDoc["error"]["message"] | "API error";
      strncpy(r.response, em, sizeof(r.response) - 1);
      r.success = false;
      goto done;
    }

    if (!llmApiDoc["choices"].is<JsonArray>() ||
        llmApiDoc["choices"].size() == 0) {
      strncpy(r.response, "Tidak ada choices", sizeof(r.response) - 1);
      r.success = false;
      goto done;
    }

    const char* contentRaw = llmApiDoc["choices"][0]["message"]["content"];
    if (!contentRaw) {
      strncpy(r.response, "Content kosong", sizeof(r.response) - 1);
      r.success = false;
      goto done;
    }

    // Salin content ke String sebelum clear doc
    String content = String(contentRaw);
    llmApiDoc.clear();  // clear SEKARANG — tidak ada pointer ke doc lagi

    Serial.printf("[LLM] Content (%u chars): %.200s\n",
                  content.length(), content.c_str());

    // ── Parse skills dari content ─────────────────────────────────────────
    // ExtractedSkill disimpan di stack llmCallTask (20KB) — aman
    ExtractedSkill extractedSkills[MAX_EXTRACTED_SKILLS];
    uint8_t        skillCount = 0;
    char           responseText[512] = "";

    // llmApiDoc sudah clear — parseLLMContent akan reuse untuk parse content
    parseLLMContent(content, extractedSkills, skillCount, responseText, sizeof(responseText));
    content = "";  // bebaskan RAM String

    strncpy(r.response, responseText, sizeof(r.response) - 1);

    // ── Dispatch skills ke skill_task via queue ───────────────────────────
    // extractedSkills sudah berisi plain char[] — tidak ada pointer ke JsonDoc
    // Aman di-pass ke dispatchSkill lintas task
    if (skillCount > 0) {
      Serial.printf("[LLM] Dispatching %u skills via queue\n", skillCount);

      // Build skill results JSON — pakai char buffer, tidak ada String concat di loop
      char srJson[512];
      int  srOff = 0;
      srOff += snprintf(srJson + srOff, sizeof(srJson) - srOff, "[");

      for (uint8_t i = 0; i < skillCount; i++) {
        SkillResponse resp;
        memset(&resp, 0, sizeof(resp));

        dispatchSkill(extractedSkills[i].tool,
                      extractedSkills[i].argsJson,
                      resp,
                      pdMS_TO_TICKS(5000));
        r.skillsExecuted++;

        Serial.printf("[LLM] Skill[%d] %s → %s: %s\n",
                      i,
                      extractedSkills[i].tool,
                      resp.success ? "OK" : "FAIL",
                      resp.message);

        // Escape double-quote di message
        char escapedMsg[128] = "";
        const char* src = resp.message;
        int ei = 0;
        while (*src && ei < (int)sizeof(escapedMsg) - 2) {
          if (*src == '"') { escapedMsg[ei++] = '\''; }
          else             { escapedMsg[ei++] = *src; }
          src++;
        }
        escapedMsg[ei] = '\0';

        srOff += snprintf(srJson + srOff, sizeof(srJson) - srOff,
                          "%s{\"tool\":\"%s\",\"success\":%s,\"message\":\"%s\"}",
                          i > 0 ? "," : "",
                          extractedSkills[i].tool,
                          resp.success ? "true" : "false",
                          escapedMsg);
      }
      snprintf(srJson + srOff, sizeof(srJson) - srOff, "]");
      strncpy(r.skillResultsJson, srJson, sizeof(r.skillResultsJson) - 1);
    }

    r.success = true;
  }

done:
  p->done = true;
  xSemaphoreGive(llmDone);
  vTaskDelete(nullptr);
}

// ─────────────────────────────────────────────────────────────────────────
// Web dashboard HTML
// ─────────────────────────────────────────────────────────────────────────

static void handleRoot() {
  server.send_P(200, "text/html", CHAT_HTML);
}

static void handleDashboardPage() {
  server.send_P(200, "text/html", DASHBOARD_HTML);
}

static void handleWifiPage() {
  server.send_P(200, "text/html", WIFI_HTML);
}

static void handleLlmPage() {
  server.send_P(200, "text/html", LLM_HTML);
}

static void handleMqttPage() {
  server.send_P(200, "text/html", MQTT_HTML);
}

static void handleSkillsPage() {
  server.send_P(200, "text/html", SKILLS_HTML);
}

// ─────────────────────────────────────────────────────────────────────────
// API handlers (dipanggil dari loop_task)
// ─────────────────────────────────────────────────────────────────────────

static void handleApiStatus() {
  sendDoc.clear();
  sendDoc["ok"]          = true;
  sendDoc["connected"]   = (WiFi.status() == WL_CONNECTED);
  sendDoc["ssid"]        = wifiConfigured ? String(wifiSSID) : "";
  if (WiFi.status() == WL_CONNECTED) sendDoc["ip"] = WiFi.localIP().toString();
  sendDoc["uptime"]      = millis() / 1000;
  sendDoc["heap"]        = ESP.getFreeHeap();
  sendDoc["min_heap"]    = ESP.getMinFreeHeap();
  sendDoc["skill_count"] = skill::skillCount();
  String json; serializeJson(sendDoc, json);
  server.send(200, "application/json", json);
}

static void handleSkillsList() {
  sendDoc.clear();
  sendDoc["ok"]     = true;
  sendDoc["skills"] = skill::listSkills();
  String json; serializeJson(sendDoc, json);
  server.send(200, "application/json", json);
}

static void handleSkillExecute() {
  if (server.method() != HTTP_POST) {
    server.send(405, "application/json", "{\"ok\":false,\"error\":\"POST only\"}");
    return;
  }

  // Parse tool + args dari body
  String body = server.arg("plain");
  recvDoc.clear();
  if (deserializeJson(recvDoc, body)) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"JSON error\"}");
    return;
  }

  const char* toolName = recvDoc["tool"] | "";
  char argsStr[SKILL_ARGS_LEN] = "{}";
  if (recvDoc["args"].is<JsonObject>()) {
    serializeJson(recvDoc["args"], argsStr, sizeof(argsStr));
  }

  SkillResponse resp;
  dispatchSkill(toolName, argsStr, resp, pdMS_TO_TICKS(5000));

  sendDoc.clear();
  sendDoc["ok"]      = resp.success;
  sendDoc["message"] = resp.message;
  String json; serializeJson(sendDoc, json);
  server.send(resp.success ? 200 : 400, "application/json", json);
}

static void handleSkillChain() {
  if (server.method() != HTTP_POST) {
    server.send(405, "application/json", "{\"ok\":false,\"error\":\"POST only\"}");
    return;
  }

  String body = server.arg("plain");
  recvDoc.clear();
  if (deserializeJson(recvDoc, body) || !recvDoc.is<JsonArray>()) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"JSON array expected\"}");
    return;
  }

  JsonArray arr    = recvDoc.as<JsonArray>();
  bool allSuccess  = true;
  String results   = "[";
  bool first       = true;

  for (JsonObject skillObj : arr) {
    const char* toolName = skillObj["tool"] | "unknown";
    char argsStr[SKILL_ARGS_LEN] = "{}";
    if (skillObj["args"].is<JsonObject>()) {
      serializeJson(skillObj["args"], argsStr, sizeof(argsStr));
    }

    SkillResponse resp;
    bool ok = dispatchSkill(toolName, argsStr, resp, pdMS_TO_TICKS(5000));
    if (!ok) allSuccess = false;

    if (!first) results += ",";
    results += "{\"tool\":\""; results += toolName;
    results += "\",\"success\":"; results += resp.success ? "true" : "false";
    results += ",\"message\":\""; results += resp.message; results += "\"}";
    first = false;
  }
  results += "]";

  sendDoc.clear();
  sendDoc["ok"]    = allSuccess;
  sendDoc["count"] = arr.size();
  String json; serializeJson(sendDoc, json);
  // Inject results array manually (sudah serialized)
  json = json.substring(0, json.length() - 1) + ",\"results\":" + results + "}";
  server.send(allSuccess ? 200 : 207, "application/json", json);
}

// handleApiChat: spawn llm_task, tunggu hasilnya
static void handleApiChat() {
  if (server.method() != HTTP_POST) {
    server.send(405, "application/json", "{\"ok\":false,\"error\":\"POST only\"}");
    return;
  }

  String body = server.arg("plain");
  recvDoc.clear();
  if (deserializeJson(recvDoc, body)) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"Bad JSON\"}");
    return;
  }
  const char* prompt = recvDoc["prompt"] | "";
  if (strlen(prompt) == 0) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"Missing prompt\"}");
    return;
  }

  // Siapkan params
  memset(&llmTaskParams, 0, sizeof(llmTaskParams));
  strncpy(llmTaskParams.prompt, prompt, sizeof(llmTaskParams.prompt) - 1);
  llmTaskParams.done = false;

  if (llmDone == nullptr) llmDone = xSemaphoreCreateBinary();

  Serial.printf("[CHAT] Prompt: %.100s  heap=%u\n",
                llmTaskParams.prompt, ESP.getFreeHeap());

  BaseType_t ok = xTaskCreatePinnedToCore(
    llmCallTask, "llm_task",
    20480,              // 20KB stack
    &llmTaskParams,
    2,                  // prioritas lebih tinggi dari wifi_task
    nullptr,
    1                   // Core 1 (App CPU)
  );

  if (ok != pdPASS) {
    Serial.printf("[LLM] Task create fail! heap=%u\n", ESP.getFreeHeap());
    server.send(500, "application/json",
                "{\"ok\":false,\"error\":\"Heap tidak cukup untuk LLM task\"}");
    return;
  }

  // Tunggu max 35 detik
  if (xSemaphoreTake(llmDone, pdMS_TO_TICKS(35000)) != pdTRUE) {
    server.send(500, "application/json", "{\"ok\":false,\"error\":\"LLM timeout\"}");
    return;
  }

  LLMResult& res = llmTaskParams.result;

  // Escape response string
  String escaped = String(res.response);
  escaped.replace("\\", "\\\\");
  escaped.replace("\"", "\\\"");
  escaped.replace("\n", "\\n");
  escaped.replace("\r", "");

  String respJson = "{\"ok\":";
  respJson += res.success ? "true" : "false";
  respJson += ",\"response\":\"";
  respJson += escaped;
  respJson += "\",\"skill_count\":";
  respJson += String(res.skillsExecuted);
  respJson += ",\"skill_results\":";
  respJson += String(res.skillResultsJson);
  respJson += "}";

  server.send(res.success ? 200 : 400, "application/json", respJson);
}

// ─────────────────────────────────────────────────────────────────────────
// API: WiFi config
// ─────────────────────────────────────────────────────────────────────────

static void handleApiWifiGet() {
  sendDoc.clear();
  sendDoc["ok"]   = true;
  sendDoc["ssid"] = wifiConfigured ? wifiSSID : "";
  sendDoc["configured"] = wifiConfigured;
  if (WiFi.status() == WL_CONNECTED) sendDoc["ip"] = WiFi.localIP().toString();
  String json; serializeJson(sendDoc, json);
  server.send(200, "application/json", json);
}

static void handleApiWifiSet() {
  if (server.method() != HTTP_POST) { server.send(405, "application/json", "{\"ok\":false}"); return; }
  String body = server.arg("plain");
  recvDoc.clear();
  if (deserializeJson(recvDoc, body)) { server.send(400, "application/json", "{\"ok\":false}"); return; }
  const char* s = recvDoc["ssid"] | "";
  const char* p = recvDoc["pass"] | "";
  if (strlen(s) == 0) { server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing_ssid\"}"); return; }
  strncpy(wifiSSID, s, sizeof(wifiSSID) - 1); wifiSSID[sizeof(wifiSSID)-1] = '\0';
  strncpy(wifiPass, p, sizeof(wifiPass) - 1); wifiPass[sizeof(wifiPass)-1] = '\0';
  saveWifiConfig(); wifiConfigured = true; wifiNeedsRetry = true;
  server.send(200, "application/json", "{\"ok\":true,\"msg\":\"wifi_saved\"}");
}

// ─────────────────────────────────────────────────────────────────────────
// API: LLM config
// ─────────────────────────────────────────────────────────────────────────

static void handleApiLlmGet() {
  sendDoc.clear();
  sendDoc["ok"]   = true;
  sendDoc["url"]  = llmUrl;
  sendDoc["model"] = llmModel;
  sendDoc["has_key"] = (strlen(llmKey) > 0);
  String json; serializeJson(sendDoc, json);
  server.send(200, "application/json", json);
}

static void handleApiLlmSet() {
  if (server.method() != HTTP_POST) { server.send(405, "application/json", "{\"ok\":false}"); return; }
  String body = server.arg("plain");
  recvDoc.clear();
  if (deserializeJson(recvDoc, body)) { server.send(400, "application/json", "{\"ok\":false}"); return; }
  const char* url   = recvDoc["url"]   | "";
  const char* key   = recvDoc["key"]   | "";
  const char* model = recvDoc["model"] | "";
  if (strlen(url)   > 0) strncpy(llmUrl,   url,   sizeof(llmUrl)   - 1);
  if (strlen(key)   > 0) strncpy(llmKey,   key,   sizeof(llmKey)   - 1);
  if (strlen(model) > 0) strncpy(llmModel, model, sizeof(llmModel) - 1);
  saveLlmConfig();
  server.send(200, "application/json", "{\"ok\":true,\"msg\":\"llm_saved\"}");
}

// ─────────────────────────────────────────────────────────────────────────
// API: MQTT config
// ─────────────────────────────────────────────────────────────────────────

static void handleApiMqttGet() {
  sendDoc.clear();
  sendDoc["ok"]     = true;
  sendDoc["host"]   = mqttHost;
  sendDoc["port"]   = mqttPort;
  sendDoc["user"]   = mqttUser;
  sendDoc["client_id"] = mqttClientId;
  sendDoc["configured"] = mqttConfigured;
  String json; serializeJson(sendDoc, json);
  server.send(200, "application/json", json);
}

static void handleApiMqttSet() {
  if (server.method() != HTTP_POST) { server.send(405, "application/json", "{\"ok\":false}"); return; }
  String body = server.arg("plain");
  recvDoc.clear();
  if (deserializeJson(recvDoc, body)) { server.send(400, "application/json", "{\"ok\":false}"); return; }
  const char* host = recvDoc["host"] | "";
  const char* user = recvDoc["user"] | "";
  const char* pass = recvDoc["pass"] | "";
  const char* cid  = recvDoc["client_id"] | "";
  int         port = recvDoc["port"] | 1883;
  if (strlen(host) > 0) strncpy(mqttHost, host, sizeof(mqttHost) - 1);
  if (strlen(user) > 0) strncpy(mqttUser, user, sizeof(mqttUser) - 1);
  if (strlen(pass) > 0) strncpy(mqttPass, pass, sizeof(mqttPass) - 1);
  if (strlen(cid)  > 0) strncpy(mqttClientId, cid, sizeof(mqttClientId) - 1);
  if (port > 0) mqttPort = (uint16_t)port;
  mqttConfigured = strlen(mqttHost) > 0;
  if (mqttConfigured) saveMqttConfig();
  server.send(200, "application/json", mqttConfigured ? "{\"ok\":true,\"msg\":\"mqtt_saved\"}" : "{\"ok\":true,\"msg\":\"mqtt_cleared\"}");
}

static void handleApiMqttSubs() {
  server.send(200, "application/json",
    "{\"ok\":true,\"subs\":" + skill::mqttSubsJson() + ",\"count\":" + String(skill::mqttActiveSubCount()) + "}");
}

static void handleApiMqttSubAdd() {
  if (server.method() != HTTP_POST) { server.send(405, "application/json", "{\"ok\":false}"); return; }
  String body = server.arg("plain");
  recvDoc.clear();
  if (deserializeJson(recvDoc, body)) { server.send(400, "application/json", "{\"ok\":false,\"error\":\"Bad JSON\"}"); return; }
  const char* topic = recvDoc["topic"] | "";
  const char* tool  = recvDoc["tool"] | "";
  const char* args  = recvDoc["args"] | "{}";
  if (strlen(topic) == 0) { server.send(400, "application/json", "{\"ok\":false,\"error\":\"Topik tidak boleh kosong\"}"); return; }
  if (strlen(tool) == 0)  { server.send(400, "application/json", "{\"ok\":false,\"error\":\"Tool tidak boleh kosong\"}"); return; }
  if (skill::mqttAddSub(topic, tool, args)) {
    skill::persistMqttSubs();
    requestMqttReconnect();
    server.send(200, "application/json", "{\"ok\":true,\"msg\":\"Subscribed: " + String(topic) + "\"}");
  } else {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"Subs penuh (max 4)\"}");
  }
}

static void handleApiMqttSubRemove() {
  if (server.method() != HTTP_POST) { server.send(405, "application/json", "{\"ok\":false}"); return; }
  String body = server.arg("plain");
  recvDoc.clear();
  if (deserializeJson(recvDoc, body)) { server.send(400, "application/json", "{\"ok\":false,\"error\":\"Bad JSON\"}"); return; }
  const char* topic = recvDoc["topic"] | "";
  bool ok = skill::mqttRemoveSub(strlen(topic) > 0 ? topic : nullptr);
  if (ok) {
    skill::persistMqttSubs();
    requestMqttReconnect();
    server.send(200, "application/json", "{\"ok\":true,\"msg\":\"Removed\"}");
  } else {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"No active subscription\"}");
  }
}

// ─────────────────────────────────────────────────────────────────────────
// Web Server Start
// ─────────────────────────────────────────────────────────────────────────

static void startWebServer() {
  if (serverStarted) return;
  server.on("/",           handleRoot);
  server.on("/dashboard",  handleDashboardPage);
  server.on("/wifi",       handleWifiPage);
  server.on("/llm",        handleLlmPage);
  server.on("/mqtt",       handleMqttPage);
  server.on("/skills",     handleSkillsPage);
  server.on("/api/status",        HTTP_GET,  handleApiStatus);
  server.on("/api/skills",        HTTP_GET,  handleSkillsList);
  server.on("/api/skill/execute", HTTP_POST, handleSkillExecute);
  server.on("/api/skill/chain",   HTTP_POST, handleSkillChain);
  server.on("/api/chat",          HTTP_POST, handleApiChat);
  server.on("/api/wifi/get",      HTTP_GET,  handleApiWifiGet);
  server.on("/api/wifi/set",      HTTP_POST, handleApiWifiSet);
  server.on("/api/llm/get",       HTTP_GET,  handleApiLlmGet);
  server.on("/api/llm/set",       HTTP_POST, handleApiLlmSet);
  server.on("/api/mqtt/get",      HTTP_GET,  handleApiMqttGet);
  server.on("/api/mqtt/set",      HTTP_POST, handleApiMqttSet);
  server.on("/api/mqtt/subs",     HTTP_GET,  handleApiMqttSubs);
  server.on("/api/mqtt/subs/add", HTTP_POST, handleApiMqttSubAdd);
  server.on("/api/mqtt/subs/remove", HTTP_POST, handleApiMqttSubRemove);
  server.begin();
  serverStarted = true;
}

// ─────────────────────────────────────────────────────────────────────────
// LLM serial commands
// ─────────────────────────────────────────────────────────────────────────

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

static void llmChat(const char* prompt) {
  if (!prompt || strlen(prompt) == 0) { jsonResult(false, "prompt_empty");       return; }
  if (WiFi.status() != WL_CONNECTED)  { jsonResult(false, "wifi_not_connected"); return; }
  if (strlen(llmKey) == 0)            { jsonResult(false, "api_key_missing");    return; }

  jsonResult(true, "llm_wait");

  memset(&llmTaskParams, 0, sizeof(llmTaskParams));
  strncpy(llmTaskParams.prompt, prompt, sizeof(llmTaskParams.prompt) - 1);
  llmTaskParams.done = false;

  if (llmDone == nullptr) llmDone = xSemaphoreCreateBinary();

  BaseType_t ok = xTaskCreatePinnedToCore(
    llmCallTask, "llm_task_s", 20480,
    &llmTaskParams, 2, nullptr, 1
  );
  if (ok != pdPASS) { jsonResult(false, "heap_insufficient"); return; }

  if (xSemaphoreTake(llmDone, pdMS_TO_TICKS(35000)) != pdTRUE) {
    jsonResult(false, "llm_timeout"); return;
  }

  LLMResult& res = llmTaskParams.result;
  sendDoc.clear();
  sendDoc["ok"]          = res.success;
  sendDoc["msg"]         = res.success ? "llm_ok" : "llm_error";
  sendDoc["reply"]       = res.response;
  sendDoc["skill_count"] = res.skillsExecuted;
  serializeJson(sendDoc, Serial);
  Serial.println();
}

// ─────────────────────────────────────────────────────────────────────────
// Serial command handler (TASK 4: loop_task, Core 0)
// ─────────────────────────────────────────────────────────────────────────

static void handleSerial() {
  static String buf;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      buf.trim();
      if (buf.length() == 0) { buf = ""; jsonResult(true, "ready"); continue; }

      recvDoc.clear();
      if (deserializeJson(recvDoc, buf)) { jsonResult(false, "bad_json"); buf = ""; continue; }

      const char* cmd = recvDoc["c"] | "";

      if (strcmp(cmd, "ws") == 0) {
        const char* s = recvDoc["s"] | "";
        const char* p = recvDoc["p"] | "";
        if (strlen(s) == 0) { jsonResult(false, "missing_ssid"); }
        else {
          strncpy(wifiSSID, s, sizeof(wifiSSID) - 1); wifiSSID[sizeof(wifiSSID)-1] = '\0';
          strncpy(wifiPass, p, sizeof(wifiPass) - 1); wifiPass[sizeof(wifiPass)-1] = '\0';
          if (saveWifiConfig()) { wifiConfigured = true; wifiNeedsRetry = true;
            jsonResult(true, "wifi_saved_connecting"); }
        }
      }
      else if (strcmp(cmd, "wst") == 0 || strcmp(cmd, "st") == 0) { jsonStatus(); }
      else if (strcmp(cmd, "wr") == 0)  { clearWifiConfig(); WiFi.disconnect(true); }
      else if (strcmp(cmd, "clr") == 0) {
        skill::clearPersistentConfig();
        jsonResult(true, "config_cleared_rebooting");
        Serial.flush(); delay(200); ESP.restart();
      }
      else if (strcmp(cmd, "rst") == 0) {
        jsonResult(true, "restarting"); Serial.flush(); delay(200); ESP.restart();
      }
      else if (strcmp(cmd, "llm_set") == 0) {
        const char* url = recvDoc["url"]   | "";
        const char* key = recvDoc["key"]   | "";
        const char* mod = recvDoc["model"] | "";
        if (strlen(url) > 0) strncpy(llmUrl,   url, sizeof(llmUrl)   - 1);
        if (strlen(key) > 0) strncpy(llmKey,   key, sizeof(llmKey)   - 1);
        if (strlen(mod) > 0) strncpy(llmModel, mod, sizeof(llmModel) - 1);
        saveLlmConfig();
        jsonResult(true, "llm_saved");
      }
      else if (strcmp(cmd, "llm_get") == 0)  { jsonLlmStatus(); }
      else if (strcmp(cmd, "llm_test") == 0) { llmTest(); }
      else if (strcmp(cmd, "mqtt_set") == 0) {
        const char* host = recvDoc["host"] | "";
        const char* user = recvDoc["user"] | "";
        const char* pass = recvDoc["pass"] | "";
        const char* cid  = recvDoc["client_id"] | "";
        int         port = recvDoc["port"] | 1883;
        if (strlen(host) > 0) strncpy(mqttHost, host, sizeof(mqttHost) - 1);
        if (strlen(user) > 0) strncpy(mqttUser, user, sizeof(mqttUser) - 1);
        if (strlen(pass) > 0) strncpy(mqttPass, pass, sizeof(mqttPass) - 1);
        if (strlen(cid)  > 0) strncpy(mqttClientId, cid, sizeof(mqttClientId) - 1);
        if (port > 0) mqttPort = (uint16_t)port;
        mqttConfigured = strlen(mqttHost) > 0;
        if (mqttConfigured) saveMqttConfig();
        jsonResult(true, mqttConfigured ? "mqtt_saved" : "mqtt_cleared");
      }
      else if (strcmp(cmd, "mqtt_get") == 0)  { jsonMqttStatus(); }
      else if (strcmp(cmd, "mqtt_test") == 0) {
        if (WiFi.status() != WL_CONNECTED) { jsonResult(false, "wifi_not_connected"); }
        else if (!mqttConfigured)          { jsonResult(false, "mqtt_not_configured"); }
        else {
          startWebServer();
          sendDoc.clear();
          sendDoc["ok"]  = true;
          sendDoc["msg"] = "mqtt_ok";
          sendDoc["ip"]  = WiFi.localIP().toString();
          serializeJson(sendDoc, Serial);
          Serial.println();
        }
      }
      else if (strcmp(cmd, "llm_chat") == 0) { llmChat(recvDoc["prompt"] | ""); }
      else if (strcmp(cmd, "start") == 0) {
        if (WiFi.status() != WL_CONNECTED) { jsonResult(false, "wifi_not_connected"); }
        else {
          startWebServer();
          sendDoc.clear();
          sendDoc["ok"]  = true;
          sendDoc["msg"] = "started";
          sendDoc["ip"]  = WiFi.localIP().toString();
          serializeJson(sendDoc, Serial);
          Serial.println();
        }
      }
      else { jsonResult(false, "unknown_cmd"); }

      buf = "";
    } else {
      if (buf.length() < 512) buf += c;
    }
  }
}

// ─────────────────────────────────────────────────────────────────────────
// Setup
// ─────────────────────────────────────────────────────────────────────────

void setup() {
  Serial.begin(115200);
  delay(800);

  hal::init();
  pinMode(27, OUTPUT);
  digitalWrite(27, LOW);
  skill::init();

  // Buat queue & semaphore sebelum task apapun dijalankan
  skillQueue = xQueueCreate(4, sizeof(SkillRequest));   // max 4 request pending
  skillRespQ = xQueueCreate(4, sizeof(SkillResponse));  // max 4 response pending
  llmDone    = xSemaphoreCreateBinary();
  wifiReady  = xSemaphoreCreateBinary();

  if (!skillQueue || !skillRespQ || !llmDone || !wifiReady) {
    Serial.println(F("[BOOT] FATAL: Queue/semaphore create failed!"));
    while (true) delay(1000);
  }

  Serial.println();
  Serial.println(F("============================="));
  Serial.println(F("  ArduClaw v0.7 Multi-Task  "));
  Serial.println(F("============================="));
  Serial.printf (  "  Free heap : %u B\n", ESP.getFreeHeap());
  Serial.println(F("  Tasks:"));
  Serial.println(F("    Core0: loop_task(16K) + skill_task(6K) + mqtt_task(6K)"));
  Serial.println(F("    Core1: wifi_task(8K)  + llm_task(20K, per-req)"));
  Serial.println(F("============================="));

  loadWifiConfig();
  loadLlmConfig();
  loadMqttConfig();

  // ── Generate random MQTT client ID ──
  if (!mqttConfigured || strlen(mqttClientId) == 0) {
    snprintf(mqttClientId, sizeof(mqttClientId), "arduclaw-%06x",
             (unsigned long)esp_random() & 0xFFFFFF);
  }

  // ── MQTT publish queue ──
  mqttPubQueue = xQueueCreate(MQTT_PUB_QUEUE_SIZE, sizeof(MqttPubRequest));

  // ── Set MQTT callbacks (dipanggil dari skill_task) ──
  skill::setMqttPublishFn(queueMqttPublish);
  skill::setMqttReconnectFn(requestMqttReconnect);

  // ── TASK: wifi_task — Core 1, 8KB ──
  xTaskCreatePinnedToCore(
    wifiTask, "wifi_task",
    8192, nullptr, 1, nullptr, 1
  );

  // ── TASK: skill_task — Core 0, 6KB ──
  // Dipin ke Core 0 agar tidak bersaing CPU dengan llm_task di Core 1
  xTaskCreatePinnedToCore(
    skillTask, "skill_task",
    6144, nullptr, 3,  // prioritas 3 = lebih tinggi dari loop, agar skill cepat
    nullptr, 0
  );

  // ── TASK: mqtt_task — Core 0, 6KB ──
  xTaskCreatePinnedToCore(
    mqttTask, "mqtt_task",
    6144, nullptr, 1, nullptr, 0
  );

  // ── Set dispatcher — WAJIB agar monitor system bisa dispatch action ──
  skill::setDispatcher(dispatchSkill);

  // ── Restore persistent config dari NVS ────────────────────────────
  // GPIO modes + monitor rules yang disimpan via chat akan dipulihkan
  // First boot = kosong, tidak ada logika default
  skill::loadPersistentConfig();

  if (mqttConfigured) {
    Serial.printf("[BOOT] MQTT configured: %s:%u as %s\n",
                  mqttHost, mqttPort, mqttClientId);
  }

  if (wifiConfigured) {
    wifiNeedsRetry = true;
    jsonResult(true, "config_loaded");
  } else {
    jsonResult(true, "ready");
  }
}

// ─────────────────────────────────────────────────────────────────────────
// Loop (loop_task, Core 0, 16KB via ARDUINO_LOOP_STACK_SIZE)
// ─────────────────────────────────────────────────────────────────────────

void loop() {
  handleSerial();
  if (serverStarted) server.handleClient();
  vTaskDelay(pdMS_TO_TICKS(10));
}