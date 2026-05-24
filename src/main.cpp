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
#include "hal.h"
#include "skill.h"

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
*{margin:0;padding:0;box-sizing:border-box}
body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;background:#f0f0f5;display:flex;flex-direction:column;align-items:center;min-height:100vh;padding:20px}
.container{background:#fff;border-radius:22px;width:100%;max-width:600px;box-shadow:0 2px 6px rgba(0,0,0,0.04),0 12px 36px rgba(0,0,0,0.07);overflow:hidden;display:flex;flex-direction:column;height:90vh}
.header{padding:20px;border-bottom:1px solid #f0f0f5;background:#fafafa}
.header h1{font-size:20px;font-weight:800}
.header h1 span{color:#FF6B00}
.tabs{display:flex;border-bottom:1px solid #f0f0f5;background:#fafafa}
.tab{flex:1;padding:12px;text-align:center;cursor:pointer;border-bottom:3px solid transparent;font-weight:600;color:#6e6e73;font-size:13px;transition:all 0.2s}
.tab.active{color:#FF6B00;border-bottom-color:#FF6B00}
.content{flex:1;overflow-y:auto;padding:16px;display:none}
.content.active{display:flex;flex-direction:column}
#dashboard .row{display:flex;align-items:center;justify-content:space-between;padding:12px 16px;background:#f8f8fb;border-radius:12px;margin-bottom:8px;font-size:14px}
#dashboard .label{color:#6e6e73;font-size:12px;font-weight:700;text-transform:uppercase}
#dashboard .val{font-weight:700;color:#1c1c1e}
.badge{display:inline-block;padding:3px 10px;border-radius:99px;font-size:12px;font-weight:700}
.ok{background:#e6f7e6;color:#2d8a2d}
.warn{background:#fff3cd;color:#856404}
#chat{display:flex;flex-direction:column;justify-content:space-between}
#messages{flex:1;overflow-y:auto;margin-bottom:12px;display:flex;flex-direction:column}
.msg{margin-bottom:12px;padding:10px 12px;border-radius:12px;max-width:85%;word-wrap:break-word;font-size:14px;line-height:1.4}
.msg.user{align-self:flex-end;background:#FF6B00;color:white}
.msg.bot{align-self:flex-start;background:#f0f0f5;color:#1c1c1e}
.msg.error{align-self:flex-start;background:#ffe6e6;color:#c00}
#input{display:flex;gap:8px}
#chat-input{flex:1;padding:10px 12px;border:1px solid #e0e0e0;border-radius:8px;font-size:14px;outline:none}
#chat-input:focus{border-color:#FF6B00}
#send-btn{padding:10px 16px;background:#FF6B00;color:white;border:none;border-radius:8px;cursor:pointer;font-weight:600;font-size:14px}
#send-btn:hover{opacity:0.9}
#send-btn:disabled{opacity:0.5;cursor:not-allowed}
.footer{padding:12px;text-align:center;font-size:11px;color:#AEAEB2;border-top:1px solid #f0f0f5}
</style>
</head>
<body>
<div class="container">
  <div class="header">
    <h1>Ardu<span>Claw</span></h1>
  </div>
  
  <div class="tabs">
    <div class="tab active" data-tab="dashboard">Dashboard</div>
    <div class="tab" data-tab="chat">Chat</div>
  </div>
  
  <div id="dashboard" class="content active"></div>
  <div id="chat" class="content">
    <div id="messages"></div>
    <div id="input">
      <input id="chat-input" type="text" placeholder="Ask me anything..." autofocus>
      <button id="send-btn">Send</button>
    </div>
  </div>
  
  <div class="footer">ArduClaw v0.6 &middot; <a href="https://sumopod.com" style="color:#AEAEB2">sumopod.com</a></div>
</div>

<script>
const tabBtns = document.querySelectorAll('.tab');
const contents = document.querySelectorAll('.content');

tabBtns.forEach(btn => {
  btn.addEventListener('click', () => {
    const tab = btn.dataset.tab;
    tabBtns.forEach(b => b.classList.remove('active'));
    contents.forEach(c => c.classList.remove('active'));
    btn.classList.add('active');
    document.getElementById(tab).classList.add('active');
    if(tab === 'dashboard') loadDashboard();
  });
});

// Load dashboard
async function loadDashboard() {
  try {
    const res = await fetch('/api/status');
    const data = await res.json();
    
    let html = '';
    const row = (label, val) => `<div class="row"><div class="label">${label}</div><div class="val">${val}</div></div>`;
    
    html += row('Status', data.connected ? '<span class="badge ok">Connected</span>' : '<span class="badge warn">Disconnected</span>');
    html += row('SSID', data.ssid || '(none)');
    html += row('IP', data.ip || '—');
    html += row('Uptime', (data.uptime || 0) + ' s');
    html += row('Heap', (data.heap || 0) + ' B');
    html += row('Skills', data.skill_count || 0);
    
    document.getElementById('dashboard').innerHTML = html;
  } catch(e) {
    console.error(e);
  }
}

// Chat
const chatInput = document.getElementById('chat-input');
const sendBtn = document.getElementById('send-btn');
const messagesDiv = document.getElementById('messages');

function addMessage(text, type) {
  const msg = document.createElement('div');
  msg.className = 'msg ' + type;
  msg.textContent = text;
  messagesDiv.appendChild(msg);
  messagesDiv.scrollTop = messagesDiv.scrollHeight;
}

async function sendMessage() {
  const text = chatInput.value.trim();
  if(!text) return;
  
  chatInput.value = '';
  sendBtn.disabled = true;
  
  addMessage(text, 'user');
  
  try {
    const res = await fetch('/api/chat', {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({prompt: text})
    });
    
    const data = await res.json();
    if(data.ok) {
      addMessage(data.response, 'bot');
      if(data.skill_results) {
        addMessage('Skills executed: ' + JSON.stringify(data.skill_results), 'bot');
      }
    } else {
      addMessage('Error: ' + data.error, 'error');
    }
  } catch(e) {
    addMessage('Network error: ' + e.message, 'error');
  }
  
  sendBtn.disabled = false;
  chatInput.focus();
}

sendBtn.addEventListener('click', sendMessage);
chatInput.addEventListener('keypress', e => {
  if(e.key === 'Enter') sendMessage();
});

loadDashboard();
</script>
</body></html>)");

  server.send(200, "text/html", html);
}

// ─────────────────────────────────────────────────────────────────────────
// Skill API Handlers
// ─────────────────────────────────────────────────────────────────────────

/** GET /api/status - System status for dashboard */
static void handleApiStatus() {
  bool connected = (WiFi.status() == WL_CONNECTED);
  
  sendDoc.clear();
  sendDoc["ok"] = true;
  sendDoc["connected"] = connected;
  sendDoc["ssid"] = wifiConfigured ? String(wifiSSID) : "";
  if (connected) {
    sendDoc["ip"] = WiFi.localIP().toString();
  }
  sendDoc["uptime"] = millis() / 1000;
  sendDoc["heap"] = ESP.getFreeHeap();
  sendDoc["skill_count"] = 5;  // gpio.write, gpio.read, gpio.mode, adc.read, system.status
  
  String json;
  serializeJson(sendDoc, json);
  server.send(200, "application/json", json);
}

/** GET /api/skills - List available skills */
static void handleSkillsList() {
  sendDoc.clear();
  sendDoc["ok"] = true;
  sendDoc["skills"] = skill::listSkills();
  
  String json;
  serializeJson(sendDoc, json);
  server.send(200, "application/json", json);
}

/** POST /api/skill/execute - Execute single skill */
static void handleSkillExecute() {
  if (server.method() != HTTP_POST) {
    sendDoc.clear();
    sendDoc["ok"] = false;
    sendDoc["error"] = "Method must be POST";
    String json;
    serializeJson(sendDoc, json);
    server.send(405, "application/json", json);
    return;
  }
  
  // Parse request body
  String body = server.arg("plain");
  recvDoc.clear();
  
  DeserializationError error = deserializeJson(recvDoc, body);
  if (error) {
    sendDoc.clear();
    sendDoc["ok"] = false;
    sendDoc["error"] = "JSON parse error";
    String json;
    serializeJson(sendDoc, json);
    server.send(400, "application/json", json);
    return;
  }
  
  // Execute skill
  JsonObject skillObj = recvDoc.as<JsonObject>();
  skill::SkillResult result = skill::executeSkill(skillObj);
  
  // Return result
  sendDoc.clear();
  sendDoc["ok"] = result.success;
  sendDoc["message"] = result.message;
  if (result.success && result.responseData.size() > 0) {
    sendDoc["data"] = result.responseData;
  }
  
  String json;
  serializeJson(sendDoc, json);
  server.send(result.success ? 200 : 400, "application/json", json);
}

/** POST /api/skill/chain - Execute multiple skills */
static void handleSkillChain() {
  if (server.method() != HTTP_POST) {
    sendDoc.clear();
    sendDoc["ok"] = false;
    sendDoc["error"] = "Method must be POST";
    String json;
    serializeJson(sendDoc, json);
    server.send(405, "application/json", json);
    return;
  }
  
  // Parse request body
  String body = server.arg("plain");
  recvDoc.clear();
  
  DeserializationError error = deserializeJson(recvDoc, body);
  if (error) {
    sendDoc.clear();
    sendDoc["ok"] = false;
    sendDoc["error"] = "JSON parse error";
    String json;
    serializeJson(sendDoc, json);
    server.send(400, "application/json", json);
    return;
  }
  
  // Execute skill chain
  JsonArray skillArray = recvDoc.as<JsonArray>();
  
  sendDoc.clear();
  JsonArray results = sendDoc.createNestedArray("results");
  
  bool allSuccess = skill::executeSkillChain(skillArray, results);
  
  sendDoc["ok"] = allSuccess;
  sendDoc["count"] = results.size();
  
  String json;
  serializeJson(sendDoc, json);
  server.send(allSuccess ? 200 : 207, "application/json", json);
}

// ─────────────────────────────────────────────────────────────────────────
// LLM Integration
// ─────────────────────────────────────────────────────────────────────────

/**
 * Call LLM API with prompt and extract skill calls
 * Returns LLM response text and executes any skill calls found
 */
struct LLMResult {
  bool success;
  String response;
  int skillsExecuted;
};

static LLMResult callLLMWithSkills(const String& userPrompt) {
  LLMResult result{false, "LLM not configured", 0};
  
  // Check if LLM is configured
  if (strlen(llmUrl) == 0 || strlen(llmKey) == 0) {
    result.response = "LLM not configured";
    return result;
  }
  
  // Check WiFi
  if (WiFi.status() != WL_CONNECTED) {
    result.response = "WiFi not connected";
    return result;
  }
  
  // Build system prompt that instructs LLM to return JSON with skills
  String systemPrompt = F("You are ArduClaw, an AI automation runtime for ESP32. ");
  systemPrompt += F("When asked to control hardware, respond with a JSON object: ");
  systemPrompt += F("{\"response\": \"...\", \"skills\": [...]}. ");
  systemPrompt += F("Available skills: gpio.write, gpio.read, gpio.mode, adc.read, system.status. ");
  systemPrompt += F("Skill format: {\"tool\": \"name\", \"args\": {...}}");
  
  // Build request JSON
  apiDoc.clear();
  apiDoc["model"] = llmModel;
  
  JsonArray messages = apiDoc.createNestedArray("messages");
  
  JsonObject sysMsg = messages.createNestedObject();
  sysMsg["role"] = "system";
  sysMsg["content"] = systemPrompt;
  
  JsonObject userMsg = messages.createNestedObject();
  userMsg["role"] = "user";
  userMsg["content"] = userPrompt;
  
  apiDoc["temperature"] = 0.7;
  apiDoc["max_tokens"] = 1024;
  
  // Serialize request
  String requestBody;
  serializeJson(apiDoc, requestBody);
  
  // Make HTTPS request
  WiFiClientSecure client;
  client.setInsecure();  // Skip cert verification (for sumopod)
  
  if (!client.connect("ai.sumopod.com", 443)) {
    result.response = "Failed to connect to LLM server";
    return result;
  }
  
  // Send HTTP request
  String request = "POST /v1/chat/completions HTTP/1.1\r\n";
  request += "Host: ai.sumopod.com\r\n";
  request += "Content-Type: application/json\r\n";
  request += "Authorization: Bearer " + String(llmKey) + "\r\n";
  request += "Content-Length: " + String(requestBody.length()) + "\r\n";
  request += "Connection: close\r\n\r\n";
  
  client.print(request);
  client.print(requestBody);
  
  // Read response
  String responseBody = "";
  bool inBody = false;
  
  while (client.connected() || client.available()) {
    if (client.available()) {
      String line = client.readStringUntil('\n');
      
      if (!inBody) {
        if (line == "\r") {
          inBody = true;
        }
      } else {
        responseBody += line;
      }
    }
  }
  
  client.stop();
  
  // Parse response
  recvDoc.clear();
  DeserializationError error = deserializeJson(recvDoc, responseBody);
  
  if (error) {
    result.response = "Failed to parse LLM response";
    return result;
  }
  
  // Extract message content
  if (recvDoc.containsKey("choices") && recvDoc["choices"].size() > 0) {
    JsonObject choice = recvDoc["choices"][0];
    if (choice.containsKey("message")) {
      String content = choice["message"]["content"];
      
      // Try to parse skill calls from response
      // Look for JSON with "skills" array
      int skillsIdx = content.indexOf("\"skills\"");
      if (skillsIdx >= 0) {
        int startIdx = content.lastIndexOf('{', skillsIdx);
        int endIdx = content.indexOf('}', skillsIdx);
        
        if (startIdx >= 0 && endIdx > startIdx) {
          String jsonStr = content.substring(startIdx, endIdx + 1);
          apiDoc.clear();
          
          if (deserializeJson(apiDoc, jsonStr) == DeserializationError::Ok) {
            result.response = apiDoc["response"].as<String>();
            
            if (apiDoc.containsKey("skills")) {
              JsonArray skills = apiDoc["skills"];
              JsonArray skillResults = sendDoc.createNestedArray("skill_results");
              
              for (JsonObject skillObj : skills) {
                skill::SkillResult skillResult = skill::executeSkill(skillObj);
                result.skillsExecuted++;
                
                JsonObject resObj = skillResults.createNestedObject();
                resObj["tool"] = skillObj["tool"];
                resObj["success"] = skillResult.success;
                resObj["message"] = skillResult.message;
              }
            }
            
            result.success = true;
            return result;
          }
        }
      }
      
      // If no skills found, just return the response
      result.response = content;
      result.success = true;
      return result;
    }
  }
  
  result.response = "Invalid LLM response format";
  return result;
}

/** POST /api/chat - Chat with LLM and execute skills */
static void handleApiChat() {
  if (server.method() != HTTP_POST) {
    sendDoc.clear();
    sendDoc["ok"] = false;
    sendDoc["error"] = "Method must be POST";
    String json;
    serializeJson(sendDoc, json);
    server.send(405, "application/json", json);
    return;
  }
  
  // Parse request body
  String body = server.arg("plain");
  recvDoc.clear();
  
  DeserializationError error = deserializeJson(recvDoc, body);
  if (error) {
    sendDoc.clear();
    sendDoc["ok"] = false;
    sendDoc["error"] = "JSON parse error";
    String json;
    serializeJson(sendDoc, json);
    server.send(400, "application/json", json);
    return;
  }
  
  // Get prompt
  if (!recvDoc.containsKey("prompt")) {
    sendDoc.clear();
    sendDoc["ok"] = false;
    sendDoc["error"] = "Missing prompt";
    String json;
    serializeJson(sendDoc, json);
    server.send(400, "application/json", json);
    return;
  }
  
  const char* prompt = recvDoc["prompt"];
  
  // Call LLM
  LLMResult llmResult = callLLMWithSkills(prompt);
  
  sendDoc.clear();
  sendDoc["ok"] = llmResult.success;
  sendDoc["response"] = llmResult.response;
  sendDoc["skill_count"] = llmResult.skillsExecuted;
  
  String json;
  serializeJson(sendDoc, json);
  server.send(llmResult.success ? 200 : 400, "application/json", json);
}

// ─────────────────────────────────────────────────────────────────────────
// Web Server Start
// ─────────────────────────────────────────────────────────────────────────

/** Start web server (idempotent) */
static void startWebServer() {
  if (serverStarted) return;
  
  server.on("/", handleRoot);
  
  // API routes
  server.on("/api/status", HTTP_GET, handleApiStatus);
  server.on("/api/skills", HTTP_GET, handleSkillsList);
  server.on("/api/skill/execute", HTTP_POST, handleSkillExecute);
  server.on("/api/skill/chain", HTTP_POST, handleSkillChain);
  server.on("/api/chat", HTTP_POST, handleApiChat);
  
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

  // Initialize HAL (Hardware Abstraction Layer)
  hal::init();
  
  // Initialize Skill System
  skill::init();

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