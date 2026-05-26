#ifndef ARDUCLAW_SKILL_H
#define ARDUCLAW_SKILL_H

#include <Arduino.h>
#include <ArduinoJson.h>

/**
 * ArduClaw Skill System v0.9.3
 *
 * SkillResponse dipindah ke sini (dari main.cpp) agar skill.cpp bisa
 * menginstansiasi struct ini secara penuh di monitorTaskFn.
 * main.cpp cukup include skill.h — hapus definisi SkillResponse dari sana.
 */

namespace skill {

// ─────────────────────────────────────────────────────────────────────────
// SkillResult — return value dari setiap skill handler
// ─────────────────────────────────────────────────────────────────────────

struct SkillResult {
  bool   success;
  String message;
};

// ─────────────────────────────────────────────────────────────────────────
// SkillResponse — hasil eksekusi yang dikirim via queue ke skill_task
// Dipindah ke sini agar visible di skill.cpp (monitorTaskFn) tanpa
// incomplete type error.
// main.cpp: hapus definisi struct SkillResponse yang ada di sana,
//           ganti dengan: using skill::SkillResponse;
// ─────────────────────────────────────────────────────────────────────────

struct SkillResponse {
  bool success;
  char message[128];
  char tool[32];
};

// ─────────────────────────────────────────────────────────────────────────
// Handler type
// ─────────────────────────────────────────────────────────────────────────

typedef SkillResult (*SkillHandler)(const JsonObject&);

// ─────────────────────────────────────────────────────────────────────────
// Kapasitas slot
// ─────────────────────────────────────────────────────────────────────────

static const uint8_t MAX_BLINK_TASKS   = 4;
static const uint8_t MAX_MONITOR_TASKS = 4;
static const uint8_t MAX_MQTT_SUBS     = 20;

// ─────────────────────────────────────────────────────────────────────────
// Dispatch callback — dipasang oleh main.cpp via skill::setDispatcher()
// ─────────────────────────────────────────────────────────────────────────

typedef bool (*DispatchFn)(const char* tool,
                           const char* argsJson,
                           SkillResponse& outResp,
                           TickType_t timeout);

void setDispatcher(DispatchFn fn);

// ─────────────────────────────────────────────────────────────────────────
// MQTT publish callback — set by main.cpp
// ─────────────────────────────────────────────────────────────────────────

typedef bool (*MqttPublishSkillFn)(const char* topic, const char* payload);
void setMqttPublishFn(MqttPublishSkillFn fn);

typedef void (*MqttReconnectFn)();
void setMqttReconnectFn(MqttReconnectFn fn);

// ─────────────────────────────────────────────────────────────────────────
// MQTT subscription slot
// ─────────────────────────────────────────────────────────────────────────

struct MqttSubSlot {
  bool        active;
  bool        brokerSynced;
  char        topic[64];
  char        actionTool[32];
  char        actionArgs[128];
};

void mqttOnMessage(const char* topic, const char* payload);
void mqttForEachActiveSub(void (*fn)(const char* topic));
void mqttSyncSubs(bool (*subFn)(const char* topic), bool (*unsubFn)(const char* topic));
void mqttMarkAllUnsynced();
bool mqttAddSub(const char* topic, const char* action, const char* actionArgs);
bool mqttRemoveSub(const char* topic);
void persistMqttSubs();
String listMqttSubs();
String mqttSubsJson();
uint8_t mqttActiveSubCount();

// ─────────────────────────────────────────────────────────────────────────
// Registry API
// ─────────────────────────────────────────────────────────────────────────

void     registerSkill(const char* name, SkillHandler handler);
bool     hasSkill(const char* name);
String   listSkills();
uint8_t  skillCount();

// ─────────────────────────────────────────────────────────────────────────
// Execution
// ─────────────────────────────────────────────────────────────────────────

SkillResult executeSkill(const JsonObject& skillJson);
SkillResult executeSkillByName(const String& name, const JsonObject& args);
bool        executeSkillChain(const JsonArray& skillArray, JsonArray& results);

// ─────────────────────────────────────────────────────────────────────────
// Init
// ─────────────────────────────────────────────────────────────────────────

void init();
bool isInitialized();

// ─────────────────────────────────────────────────────────────────────────
// Persistent config (NVS) — monitor rules & GPIO modes awet setelah reboot
// ─────────────────────────────────────────────────────────────────────────

uint8_t loadPersistentConfig();   // restore from NVS, return restored monitor count
bool    hasPersistentConfig();    // true jika pernah dikonfigurasi via chat
void    clearPersistentConfig();  // kosongkan config tapi tetap tandai "pernah dikonfigurasi"
void    resetFactoryConfig();     // hapus SEMUA key NVS — boot berikutnya load factory defaults

}  // namespace skill

#endif  // ARDUCLAW_SKILL_H