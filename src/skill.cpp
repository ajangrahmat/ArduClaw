#include "skill.h"
#include "hal.h"
#include <Preferences.h>
#include <WiFi.h>
#include <esp_sleep.h>
#include <LittleFS.h>

/**
 * ArduClaw Skill System v0.9.2
 *
 * Fix linker error "undefined reference to dispatchSkill":
 *  - Hapus `extern dispatchSkill` — tidak bisa link ke fungsi static di main.cpp
 *  - Ganti dengan DispatchFn callback yang dipasang via skill::setDispatcher()
 *  - main.cpp memanggil skill::setDispatcher(dispatchSkill) setelah queue dibuat,
 *    sebelum skill::init() dipanggil
 *
 * Fix warnings ArduinoJson v7:
 *  - containsKey("value") → args["value"].is<JsonVariant>()
 *  - containsKey("tool")  → args["tool"].is<const char*>()
 *  - StaticJsonDocument   → JsonDocument
 */

namespace skill {

// ─────────────────────────────────────────────────────────────────────────
// DispatchFn callback — dipasang dari main.cpp
// ─────────────────────────────────────────────────────────────────────────

static DispatchFn s_dispatch = nullptr;

void setDispatcher(DispatchFn fn) {
  s_dispatch = fn;
}

// ─────────────────────────────────────────────────────────────────────────
// MQTT publish callback — dipasang dari main.cpp
// ─────────────────────────────────────────────────────────────────────────

static MqttPublishSkillFn mqttPublishFn = nullptr;

void setMqttPublishFn(MqttPublishSkillFn fn) {
  mqttPublishFn = fn;
}

static MqttReconnectFn mqttReconnectFn = nullptr;

void setMqttReconnectFn(MqttReconnectFn fn) {
  mqttReconnectFn = fn;
}

// Forward declarations untuk NVS persistence (dipanggil dari handler)
static void persistGpioMode(uint8_t pin, const char* mode);
static void persistAllMonitors();

// ─────────────────────────────────────────────────────────────────────────
// Registry
// ─────────────────────────────────────────────────────────────────────────

static const uint8_t MAX_SKILLS = 30;

struct SkillEntry {
  char         name[32];
  SkillHandler handler;
};

static SkillEntry registry[MAX_SKILLS];
static uint8_t    regCount     = 0;
static bool       _initialized = false;

// ─────────────────────────────────────────────────────────────────────────
// Blink task state
// ─────────────────────────────────────────────────────────────────────────

struct BlinkSlot {
  volatile bool  active;
  volatile bool  stopRequest;
  uint8_t        pin;
  uint32_t       onMs;
  uint32_t       offMs;
  TaskHandle_t   handle;
};

static BlinkSlot blinkSlots[MAX_BLINK_TASKS];
static bool      blinkSlotsInited = false;

static void initBlinkSlots() {
  if (blinkSlotsInited) return;
  for (uint8_t i = 0; i < MAX_BLINK_TASKS; i++) {
    blinkSlots[i].active      = false;
    blinkSlots[i].stopRequest = false;
    blinkSlots[i].pin         = 255;
    blinkSlots[i].handle      = nullptr;
  }
  blinkSlotsInited = true;
}

static void blinkTaskFn(void* param) {
  BlinkSlot* slot = (BlinkSlot*)param;
  Serial.printf("[BLINK] Task start pin=%d on=%ums off=%ums\n",
                slot->pin, slot->onMs, slot->offMs);
  hal::gpioSetMode(slot->pin, hal::PinMode::PIN_OUTPUT);
  while (!slot->stopRequest) {
    hal::gpioWrite(slot->pin, true);
    vTaskDelay(pdMS_TO_TICKS(slot->onMs));
    if (slot->stopRequest) break;
    hal::gpioWrite(slot->pin, false);
    vTaskDelay(pdMS_TO_TICKS(slot->offMs));
  }
  hal::gpioWrite(slot->pin, false);
  Serial.printf("[BLINK] Task stop pin=%d\n", slot->pin);
  slot->active      = false;
  slot->stopRequest = false;
  slot->handle      = nullptr;
  vTaskDelete(nullptr);
}

static BlinkSlot* findBlinkSlotByPin(uint8_t pin) {
  for (uint8_t i = 0; i < MAX_BLINK_TASKS; i++)
    if (blinkSlots[i].active && blinkSlots[i].pin == pin)
      return &blinkSlots[i];
  return nullptr;
}

static BlinkSlot* findFreeBlinkSlot() {
  for (uint8_t i = 0; i < MAX_BLINK_TASKS; i++)
    if (!blinkSlots[i].active)
      return &blinkSlots[i];
  return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────
// Monitor task state
//
// PENTING: enum value tidak boleh bernama CHANGE — Arduino.h mendefinisikan
//   #define CHANGE 0x03
// yang akan merusak enum class. Semua value diberi prefix TRIG_.
// ─────────────────────────────────────────────────────────────────────────

enum class MonitorTrigger : uint8_t {
  TRIG_LOW,     // fire pada tepi turun HIGH→LOW (pull-up button ditekan)
  TRIG_HIGH,    // fire pada tepi naik  LOW→HIGH (sensor active-high)
  TRIG_CHANGE,  // fire setiap perubahan state
};

static const uint8_t MONITOR_TOOL_LEN = 32;
static const uint8_t MONITOR_ARGS_LEN = 128;

struct MonitorSlot {
  volatile bool  active;
  volatile bool  stopRequest;
  uint8_t        pin;
  MonitorTrigger trigger;
  char           actionTool[MONITOR_TOOL_LEN];
  char           actionArgs[MONITOR_ARGS_LEN];
  uint32_t       pollMs;
  uint32_t       debounceMs;
  bool           once;
  TaskHandle_t   handle;
};

static MonitorSlot monitorSlots[MAX_MONITOR_TASKS];
static bool        monitorSlotsInited = false;

static void initMonitorSlots() {
  if (monitorSlotsInited) return;
  for (uint8_t i = 0; i < MAX_MONITOR_TASKS; i++) {
    monitorSlots[i].active      = false;
    monitorSlots[i].stopRequest = false;
    monitorSlots[i].pin         = 255;
    monitorSlots[i].handle      = nullptr;
  }
  monitorSlotsInited = true;
}

static void monitorTaskFn(void* param) {
  MonitorSlot* slot = (MonitorSlot*)param;

  Serial.printf("[MONITOR] Task start pin=%d trigger=%d action=%s\n",
                slot->pin, (int)slot->trigger, slot->actionTool);

  hal::gpioSetMode(slot->pin, hal::PinMode::PIN_INPUT_PULLUP);

  bool     lastState        = false;
  bool     candidateState   = false;
  uint32_t candidateStartMs = 0;
  hal::gpioRead(slot->pin, lastState);
  candidateState = lastState;

  while (!slot->stopRequest) {
    bool currentRaw = false;
    hal::gpioRead(slot->pin, currentRaw);

    // ── Debounce ─────────────────────────────────────────────────────────
    if (currentRaw != candidateState) {
      candidateState   = currentRaw;
      candidateStartMs = millis();
    }
    bool stableState = lastState;
    if (millis() - candidateStartMs >= slot->debounceMs) {
      stableState = candidateState;
    }

    // ── Cek trigger ──────────────────────────────────────────────────────
    bool shouldFire = false;
    if (stableState != lastState) {
      switch (slot->trigger) {
        case MonitorTrigger::TRIG_LOW:
          shouldFire = (stableState == false);  // HIGH→LOW
          break;
        case MonitorTrigger::TRIG_HIGH:
          shouldFire = (stableState == true);   // LOW→HIGH
          break;
        case MonitorTrigger::TRIG_CHANGE:
          shouldFire = true;
          break;
      }
      lastState = stableState;
    }

    if (shouldFire) {
      Serial.printf("[MONITOR] Trigger! pin=%d state=%s action=%s\n",
                    slot->pin, stableState ? "HIGH" : "LOW", slot->actionTool);

      // Panggil via callback — tidak ada extern linkage ke main.cpp
      if (s_dispatch != nullptr) {
        SkillResponse resp;
        memset(&resp, 0, sizeof(resp));
        bool ok = s_dispatch(slot->actionTool, slot->actionArgs, resp,
                              pdMS_TO_TICKS(5000));
        Serial.printf("[MONITOR] Action %s → %s: %s\n",
                      slot->actionTool, ok ? "OK" : "FAIL", resp.message);
      } else {
        Serial.println(F("[MONITOR] WARN: dispatcher belum di-set!"));
      }

      if (slot->once) {
        Serial.printf("[MONITOR] once=true, stop pin=%d\n", slot->pin);
        break;
      }
    }

    vTaskDelay(pdMS_TO_TICKS(slot->pollMs));
  }

  Serial.printf("[MONITOR] Task stop pin=%d\n", slot->pin);
  slot->active      = false;
  slot->stopRequest = false;
  slot->handle      = nullptr;
  vTaskDelete(nullptr);
}

static MonitorSlot* findMonitorSlotByPin(uint8_t pin) {
  for (uint8_t i = 0; i < MAX_MONITOR_TASKS; i++)
    if (monitorSlots[i].active && monitorSlots[i].pin == pin)
      return &monitorSlots[i];
  return nullptr;
}

static MonitorSlot* findMonitorSlotByPinAndTrigger(uint8_t pin, MonitorTrigger trigger) {
  for (uint8_t i = 0; i < MAX_MONITOR_TASKS; i++)
    if (monitorSlots[i].active && monitorSlots[i].pin == pin && monitorSlots[i].trigger == trigger)
      return &monitorSlots[i];
  return nullptr;
}

static MonitorSlot* findFreeMonitorSlot() {
  for (uint8_t i = 0; i < MAX_MONITOR_TASKS; i++)
    if (!monitorSlots[i].active)
      return &monitorSlots[i];
  return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────
// MQTT subscription state
// ─────────────────────────────────────────────────────────────────────────

static MqttSubSlot mqttSubSlots[MAX_MQTT_SUBS];
static bool        mqttSubSlotsInited = false;

static void initMqttSubSlots() {
  if (mqttSubSlotsInited) return;
  for (uint8_t i = 0; i < MAX_MQTT_SUBS; i++) {
    mqttSubSlots[i].active       = false;
    mqttSubSlots[i].brokerSynced = false;
    mqttSubSlots[i].topic[0]     = '\0';
    mqttSubSlots[i].actionTool[0] = '\0';
    mqttSubSlots[i].actionArgs[0] = '\0';
  }
  mqttSubSlotsInited = true;
}

// ─────────────────────────────────────────────────────────────────────────
// MQTT topic matching (MQTT wildcard: + = one level, # = rest)
// ─────────────────────────────────────────────────────────────────────────

static bool topicMatches(const char* subTopic, const char* incomingTopic) {
  const char* s = subTopic;
  const char* i = incomingTopic;

  while (*s) {
    if (*s == '#') {
      // '#' must be the last character
      return *(s + 1) == '\0';
    }
    if (*s == '+') {
      // '+' matches exactly one level
      s++;
      // skip incoming until '/'
      while (*i && *i != '/') i++;
      if (*s == '\0') {
        // sub ends with '+', incoming must also end
        return *i == '\0';
      }
      if (*s == '/') {
        s++;
        if (*i == '/') i++;
        else return false;
      } else {
        return false;
      }
      continue;
    }
    if (*s != *i) return false;
    s++;
    i++;
  }
  return *i == '\0';
}

// ─────────────────────────────────────────────────────────────────────────
// Strapping pin check
// ─────────────────────────────────────────────────────────────────────────

static const uint8_t STRAPPING_PINS[] = {0, 2, 5, 12, 15};
static const uint8_t STRAPPING_COUNT  = 5;

static bool isStrappingPin(uint8_t pin) {
  for (uint8_t i = 0; i < STRAPPING_COUNT; i++)
    if (STRAPPING_PINS[i] == pin) return true;
  return false;
}

// ─────────────────────────────────────────────────────────────────────────
// NVS key constants (must be before any handler that uses them)
// ─────────────────────────────────────────────────────────────────────────

static const char* NVS_NS  = "ac";
static const char* KEY_MON = "mons";
static const char* KEY_BLK = "blks";
static const char* KEY_GIO = "gpiocfg";
static const char* KEY_GPO = "gpioout";
static const char* KEY_MQT = "mqts";
static const char* KEY_SET = "cfg_set";
static const char* KEY_PWM = "pwms";
static const char* KEY_DBN = "dbns";
static const char* KEY_POC = "pocs";
static const char* KEY_FIL = "files";

// ─────────────────────────────────────────────────────────────────────────
// PWM slot (LEDC)
// ─────────────────────────────────────────────────────────────────────────

#define MAX_PWM_CHANNELS 8

struct PwmSlot {
  bool     active;
  uint8_t  pin;
  uint8_t  channel;
  uint32_t freq;
  uint8_t  duty;
};

static PwmSlot pwmSlots[MAX_PWM_CHANNELS];
static bool    pwmInited = false;

static void initPwmSlots() {
  if (pwmInited) return;
  for (uint8_t i = 0; i < MAX_PWM_CHANNELS; i++) {
    pwmSlots[i].active  = false;
    pwmSlots[i].pin     = 255;
  }
  pwmInited = true;
}

static int8_t pwmAllocChannel(uint8_t pin) {
  for (uint8_t i = 0; i < MAX_PWM_CHANNELS; i++)
    if (!pwmSlots[i].active) return (int8_t)i;
  return -1;
}

static int8_t pwmFindByPin(uint8_t pin) {
  for (uint8_t i = 0; i < MAX_PWM_CHANNELS; i++)
    if (pwmSlots[i].active && pwmSlots[i].pin == pin) return (int8_t)i;
  return -1;
}

static void pwmDetachPin(uint8_t pin) {
  int8_t idx = pwmFindByPin(pin);
  if (idx >= 0) {
    ledcDetachPin(pin);
    pwmSlots[idx].active = false;
    pwmSlots[idx].pin    = 255;
  }
}

// ─────────────────────────────────────────────────────────────────────────
// Servo slot (via LEDC, 50Hz PWM)
// ─────────────────────────────────────────────────────────────────────────

#define MAX_SERVOS 4
#define SERVO_BASE_CH 10  // LEDC channels 10-13 for servos

struct ServoSlot {
  bool    active;
  uint8_t pin;
  uint8_t channel;
};

static ServoSlot servoSlots[MAX_SERVOS];
static bool     servosInited = false;

static void initServoSlots() {
  if (servosInited) return;
  for (uint8_t i = 0; i < MAX_SERVOS; i++)
    servoSlots[i].active = false;
  servosInited = true;
}

static int8_t servoFindFree() {
  for (uint8_t i = 0; i < MAX_SERVOS; i++)
    if (!servoSlots[i].active) return (int8_t)i;
  return -1;
}

static int8_t servoFindByPin(uint8_t pin) {
  for (uint8_t i = 0; i < MAX_SERVOS; i++)
    if (servoSlots[i].active && servoSlots[i].pin == pin) return (int8_t)i;
  return -1;
}

static uint32_t servoAngleToDuty(int angle) {
  // 50Hz, 12-bit: period = 20ms, 0°=1ms, 180°=2ms
  // duty = (minPulse + angle/180 * range) / period * 4096
  // 1ms = 205, 2ms = 410
  return (uint32_t)(205 + (uint32_t)angle * 205 / 180);
}

// ─────────────────────────────────────────────────────────────────────────
// Debounce slot
// ─────────────────────────────────────────────────────────────────────────

#define MAX_DEBOUNCE 4

struct DebounceSlot {
  volatile bool  active;
  volatile bool  stopRequest;
  uint8_t        pin;
  uint32_t       ms;
  char           actionTool[32];
  char           actionArgs[128];
  TaskHandle_t   handle;
};

static DebounceSlot debounceSlots[MAX_DEBOUNCE];
static bool         debounceInited = false;

static void initDebounceSlots() {
  if (debounceInited) return;
  for (uint8_t i = 0; i < MAX_DEBOUNCE; i++) {
    debounceSlots[i].active      = false;
    debounceSlots[i].stopRequest = false;
    debounceSlots[i].pin         = 255;
    debounceSlots[i].handle      = nullptr;
  }
  debounceInited = true;
}

static void debounceTaskFn(void* param) {
  DebounceSlot* slot = (DebounceSlot*)param;
  hal::gpioSetMode(slot->pin, hal::PinMode::PIN_INPUT_PULLUP);
  bool lastState = false;
  hal::gpioRead(slot->pin, lastState);
  bool candidate  = lastState;
  uint32_t candMs = 0;

  while (!slot->stopRequest) {
    bool cur = false;
    hal::gpioRead(slot->pin, cur);
    if (cur != candidate) { candidate = cur; candMs = millis(); }
    if (millis() - candMs >= slot->ms && candidate != lastState) {
      lastState = candidate;
      if (s_dispatch) {
        SkillResponse resp;
        memset(&resp, 0, sizeof(resp));
        s_dispatch(slot->actionTool, slot->actionArgs, resp, pdMS_TO_TICKS(5000));
      }
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  slot->active = false; slot->stopRequest = false; slot->handle = nullptr;
  vTaskDelete(nullptr);
}

static DebounceSlot* debounceFindByPin(uint8_t pin) {
  for (uint8_t i = 0; i < MAX_DEBOUNCE; i++)
    if (debounceSlots[i].active && debounceSlots[i].pin == pin) return &debounceSlots[i];
  return nullptr;
}

static DebounceSlot* debounceFindFree() {
  for (uint8_t i = 0; i < MAX_DEBOUNCE; i++)
    if (!debounceSlots[i].active) return &debounceSlots[i];
  return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────
// Publish-on-change slot (MQTT auto-publish pin changes)
// ─────────────────────────────────────────────────────────────────────────

#define MAX_PUBCHANGE 4

struct PubChangeSlot {
  volatile bool  active;
  volatile bool  stopRequest;
  uint8_t        pin;
  char           topic[64];
  char           highMsg[64];
  char           lowMsg[64];
  TaskHandle_t   handle;
};

static PubChangeSlot pubChangeSlots[MAX_PUBCHANGE];
static bool          pubChangeInited = false;

static void initPubChangeSlots() {
  if (pubChangeInited) return;
  for (uint8_t i = 0; i < MAX_PUBCHANGE; i++) {
    pubChangeSlots[i].active      = false;
    pubChangeSlots[i].stopRequest = false;
    pubChangeSlots[i].pin         = 255;
    pubChangeSlots[i].handle      = nullptr;
  }
  pubChangeInited = true;
}

static void pubChangeTaskFn(void* param) {
  PubChangeSlot* slot = (PubChangeSlot*)param;
  hal::gpioSetMode(slot->pin, hal::PinMode::PIN_INPUT_PULLUP);
  bool last = false;
  hal::gpioRead(slot->pin, last);
  while (!slot->stopRequest) {
    vTaskDelay(pdMS_TO_TICKS(50));
    bool cur = false;
    hal::gpioRead(slot->pin, cur);
    if (cur != last) {
      last = cur;
      const char* payload = cur ? slot->highMsg : slot->lowMsg;
      if (mqttPublishFn) {
        if (!mqttPublishFn(slot->topic, payload))
          Serial.printf("[PUBCHANGE] MQTT pub fail pin=%d topic=%s\n", slot->pin, slot->topic);
      }
    }
  }
  slot->active = false; slot->stopRequest = false; slot->handle = nullptr;
  vTaskDelete(nullptr);
}

static PubChangeSlot* pubChangeFindByPin(uint8_t pin) {
  for (uint8_t i = 0; i < MAX_PUBCHANGE; i++)
    if (pubChangeSlots[i].active && pubChangeSlots[i].pin == pin) return &pubChangeSlots[i];
  return nullptr;
}

static PubChangeSlot* pubChangeFindFree() {
  for (uint8_t i = 0; i < MAX_PUBCHANGE; i++)
    if (!pubChangeSlots[i].active) return &pubChangeSlots[i];
  return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────
// GPIO Skills
// ─────────────────────────────────────────────────────────────────────────

static SkillResult gpioWriteHandler(const JsonObject& args) {
  if (!args["pin"].is<int>() || !args["value"].is<JsonVariant>())
    return {false, "Missing pin or value"};
  uint8_t pin = args["pin"].as<uint8_t>();
  bool    val;
  if (args["value"].is<bool>()) {
    val = args["value"].as<bool>();
  } else if (args["value"].is<const char*>()) {
    const char* s = args["value"];
    // Coba parse sebagai integer — jika numerik murni, treat >0 = ON, 0 = OFF
    char* end = nullptr;
    long num = strtol(s, &end, 10);
    if (end != s && *end == '\0') {
      val = (num != 0);
    } else {
      val = (strcmp(s, "on") == 0 || strcmp(s, "ON") == 0 ||
             strcmp(s, "high") == 0 || strcmp(s, "HIGH") == 0 ||
             strcmp(s, "1") == 0 || strcmp(s, "true") == 0);
    }
  } else {
    val = (args["value"].as<int>() != 0);
  }
  String warn = "";
  if (isStrappingPin(pin)) {
    warn = " ⚠️Strapping pin!";
    Serial.printf("[WARN] gpio.write: GPIO %d adalah strapping pin!\n", pin);
  }
  BlinkSlot* existing = findBlinkSlotByPin(pin);
  if (existing) {
    existing->stopRequest = true;
    for (int i = 0; i < 20 && existing->active; i++) vTaskDelay(pdMS_TO_TICKS(50));
  }
  hal::gpioSetMode(pin, hal::PinMode::PIN_OUTPUT);
  if (hal::gpioWrite(pin, val)) {
    // Persist output value
    {
      Preferences prefs;
      prefs.begin(NVS_NS, false);
      prefs.putUChar(KEY_SET, 1);
      String raw = prefs.getString(KEY_GPO, "{}");
      JsonDocument doc;
      if (raw.length() > 0) deserializeJson(doc, raw);
      doc[String(pin)] = val ? 1 : 0;
      String json; serializeJson(doc, json);
      prefs.putString(KEY_GPO, json);
      prefs.end();
    }
    Serial.printf("[SKILL] gpio.write pin=%d → %s\n", pin, val ? "HIGH" : "LOW");
    return {true, "GPIO " + String(pin) + " → " + (val ? "HIGH (ON)" : "LOW (OFF)") + warn};
  }
  return {false, "Gagal write GPIO " + String(pin)};
}

static SkillResult gpioReadHandler(const JsonObject& args) {
  if (!args["pin"].is<int>()) return {false, "Missing pin"};
  uint8_t pin   = args["pin"].as<uint8_t>();
  bool    value = false;
  hal::gpioRead(pin, value);
  Serial.printf("[SKILL] gpio.read pin=%d = %s\n", pin, value ? "HIGH" : "LOW");
  return {true, "GPIO " + String(pin) + " = " + (value ? "HIGH" : "LOW")};
}

static SkillResult gpioModeHandler(const JsonObject& args) {
  if (!args["pin"].is<int>() || !args["mode"].is<const char*>())
    return {false, "Missing pin or mode"};
  uint8_t     pin     = args["pin"].as<uint8_t>();
  const char* modeStr = args["mode"] | "input";
  String warn = "";
  if (isStrappingPin(pin)) {
    warn = " ⚠️Strapping pin!";
    Serial.printf("[WARN] gpio.mode: GPIO %d adalah strapping pin!\n", pin);
  }
  hal::PinMode mode = hal::PinMode::PIN_INPUT;
  if      (strcmp(modeStr, "output")         == 0) mode = hal::PinMode::PIN_OUTPUT;
  else if (strcmp(modeStr, "input_pullup")   == 0) mode = hal::PinMode::PIN_INPUT_PULLUP;
  else if (strcmp(modeStr, "input_pulldown") == 0) mode = hal::PinMode::PIN_INPUT_PULLDOWN;
  if (hal::gpioSetMode(pin, mode)) {
    Serial.printf("[SKILL] gpio.mode pin=%d mode=%s\n", pin, modeStr);
    persistGpioMode(pin, modeStr);
    return {true, "GPIO " + String(pin) + " mode=" + String(modeStr) + warn};
  }
  return {false, "Gagal set mode GPIO " + String(pin)};
}

static SkillResult gpioBlinkHandler(const JsonObject& args) {
  if (!args["pin"].is<int>())
    return {false, "Missing pin"};
  uint8_t pin = args["pin"].as<uint8_t>();
  int on_ms   = args["on_ms"]  | 500;
  int off_ms  = args["off_ms"] | 500;
  int count   = args["count"]  | 1;
  String warn = "";
  if (isStrappingPin(pin)) {
    warn = " ⚠️Strapping pin!";
    Serial.printf("[WARN] gpio.blink: GPIO %d adalah strapping pin!\n", pin);
  }
  BlinkSlot* slot = findFreeBlinkSlot();
  if (!slot) return {false, "Blink slot penuh (max 4)"};
  slot->pin     = pin;
  slot->onMs    = on_ms;
  slot->offMs   = off_ms;
  slot->active  = true;
  slot->stopRequest = false;
  hal::gpioSetMode(pin, hal::PinMode::PIN_OUTPUT);
  return {true, "Blink " + String(pin) + " " + String(count) + "x " + String(on_ms) + "/" + String(off_ms) + warn};
}

static SkillResult gpioBlinkStartHandler(const JsonObject& args) {
  if (!args["pin"].is<int>())
    return {false, "Missing pin"};
  uint8_t pin  = args["pin"].as<uint8_t>();
  int on_ms    = args["on_ms"]  | 500;
  int off_ms   = args["off_ms"] | 500;
  String warn = "";
  if (isStrappingPin(pin)) {
    warn = " ⚠️Strapping pin!";
    Serial.printf("[WARN] gpio.blink_start: GPIO %d adalah strapping pin!\n", pin);
  }
  BlinkSlot* existing = findBlinkSlotByPin(pin);
  if (existing) {
    existing->stopRequest = true;
    for (int i = 0; i < 20 && existing->active; i++) vTaskDelay(pdMS_TO_TICKS(50));
  }
  BlinkSlot* slot = findFreeBlinkSlot();
  if (!slot)
    return {false, "Blink slot penuh (max " + String(MAX_BLINK_TASKS) + " pin)"};
  slot->pin = pin; slot->onMs = on_ms; slot->offMs = off_ms;
  slot->stopRequest = false; slot->active = true;
  BaseType_t ok = xTaskCreatePinnedToCore(
    blinkTaskFn, "blink_task", 2048, (void*)slot, 2, &slot->handle, 0);
  if (ok != pdPASS) {
    slot->active = false; slot->handle = nullptr;
    return {false, "Gagal buat blink task (heap " + String(ESP.getFreeHeap()) + "B)"};
  }
  // Persist blink config
  {
    Preferences prefs;
    prefs.begin(NVS_NS, false);
    prefs.putUChar(KEY_SET, 1);
    String raw = prefs.getString(KEY_BLK, "[]");
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, raw);
    JsonArray arr;
    if (err || !doc.is<JsonArray>()) arr = doc.to<JsonArray>();
    else arr = doc.as<JsonArray>();
    // Remove existing entry for this pin, then add
    JsonArray filtered;
    filtered = arr; // reuse
    // Build new array without this pin
    JsonDocument nd;
    JsonArray na = nd.to<JsonArray>();
    for (JsonObject b : arr) {
      if (b["pin"].as<uint8_t>() != pin)
        na.add(b);
    }
    JsonObject nb = na.add<JsonObject>();
    nb["pin"]    = pin;
    nb["on_ms"]  = on_ms;
    nb["off_ms"] = off_ms;
    String json; serializeJson(na, json);
    prefs.putString(KEY_BLK, json);
    prefs.end();
  }
  char msg[80];
  snprintf(msg, sizeof(msg), "GPIO %d blink dimulai (on=%ums off=%ums)", pin, on_ms, off_ms);
  Serial.printf("[SKILL] gpio.blink_start: %s\n", msg);
  return {true, String(msg) + warn};
}

static void removeBlinkFromNvs(uint8_t pin) {
  Preferences prefs;
  prefs.begin(NVS_NS, false);
  String raw = prefs.getString(KEY_BLK, "[]");
  JsonDocument doc;
  if (deserializeJson(doc, raw) != DeserializationError::Ok || !doc.is<JsonArray>()) {
    prefs.end();
    return;
  }
  JsonDocument nd;
  JsonArray na = nd.to<JsonArray>();
  for (JsonObject b : doc.as<JsonArray>()) {
    if (b["pin"].as<uint8_t>() != pin) na.add(b);
  }
  String json; serializeJson(na, json);
  prefs.putString(KEY_BLK, json);
  prefs.end();
}

static SkillResult gpioBlinkStopHandler(const JsonObject& args) {
  if (!args["pin"].is<int>()) {
    uint8_t stopped = 0;
    for (uint8_t i = 0; i < MAX_BLINK_TASKS; i++) {
      if (blinkSlots[i].active) { blinkSlots[i].stopRequest = true; stopped++; }
    }
    for (int w = 0; w < 40; w++) {
      bool any = false;
      for (uint8_t i = 0; i < MAX_BLINK_TASKS; i++)
        if (blinkSlots[i].active) { any = true; break; }
      if (!any) break;
      vTaskDelay(pdMS_TO_TICKS(50));
    }
    Preferences prefs;
    prefs.begin(NVS_NS, false);
    prefs.putString(KEY_BLK, "[]");
    prefs.end();
    char msg[48];
    snprintf(msg, sizeof(msg), "Semua blink task dihentikan (%u task)", stopped);
    return {stopped > 0, String(msg)};
  }
  uint8_t    pin = args["pin"].as<uint8_t>();
  BlinkSlot* s   = findBlinkSlotByPin(pin);
  if (!s) return {false, "Tidak ada blink task aktif di pin " + String(pin)};
  s->stopRequest = true;
  for (int i = 0; i < 30 && s->active; i++) vTaskDelay(pdMS_TO_TICKS(50));
  removeBlinkFromNvs(pin);
  char msg[48];
  snprintf(msg, sizeof(msg), "GPIO %d blink dihentikan", pin);
  return {true, String(msg)};
}

// ─────────────────────────────────────────────────────────────────────────
// gpio.monitor
// ─────────────────────────────────────────────────────────────────────────

static SkillResult gpioMonitorHandler(const JsonObject& args) {
  if (!args["pin"].is<int>())
    return {false, "Missing pin"};
  if (!args["action"].is<const char*>() || strlen(args["action"] | "") == 0)
    return {false, "Missing action"};

  uint8_t     pin       = args["pin"].as<uint8_t>();
  const char* actionStr = args["action"] | "";
  uint32_t    pollMs    = args["poll_ms"].is<int>()     ? (uint32_t)args["poll_ms"].as<int>()     : 50;
  uint32_t    debMs     = args["debounce_ms"].is<int>() ? (uint32_t)args["debounce_ms"].as<int>() : 30;
  bool        once      = args["once"].is<bool>()       ? args["once"].as<bool>()                 : false;

  if (pollMs < 10) pollMs = 10;
  if (debMs  < 5)  debMs  = 5;

  const char*    trigStr = args["trigger"] | "low";
  MonitorTrigger trigger = MonitorTrigger::TRIG_LOW;
  if      (strcmp(trigStr, "high")   == 0) trigger = MonitorTrigger::TRIG_HIGH;
  else if (strcmp(trigStr, "change") == 0) trigger = MonitorTrigger::TRIG_CHANGE;

  if (!hasSkill(actionStr))
    return {false, "Action skill tidak ditemukan: " + String(actionStr)};

  if (s_dispatch == nullptr)
    return {false, "Dispatcher belum di-set. Panggil skill::setDispatcher() dari setup()"};

  MonitorSlot* existing = findMonitorSlotByPinAndTrigger(pin, trigger);
  if (existing) {
    Serial.printf("[MONITOR] Pin %d trigger=%s sudah ada, stop dulu\n", pin, trigStr);
    existing->stopRequest = true;
    for (int i = 0; i < 30 && existing->active; i++) vTaskDelay(pdMS_TO_TICKS(50));
  }

  MonitorSlot* slot = findFreeMonitorSlot();
  if (!slot)
    return {false, "Monitor slot penuh (max " + String(MAX_MONITOR_TASKS) + " pin)"};

  slot->pin         = pin;
  slot->trigger     = trigger;
  slot->pollMs      = pollMs;
  slot->debounceMs  = debMs;
  slot->once        = once;
  slot->stopRequest = false;
  slot->active      = true;

  strncpy(slot->actionTool, actionStr, sizeof(slot->actionTool) - 1);
  slot->actionTool[sizeof(slot->actionTool) - 1] = '\0';

  if (args["action_args"].is<JsonObject>()) {
    serializeJson(args["action_args"], slot->actionArgs, sizeof(slot->actionArgs));
  } else {
    strncpy(slot->actionArgs, "{}", sizeof(slot->actionArgs) - 1);
  }

  BaseType_t ok = xTaskCreatePinnedToCore(
    monitorTaskFn, "monitor_task",
    3072, (void*)slot, 2, &slot->handle, 0);

  if (ok != pdPASS) {
    slot->active = false; slot->handle = nullptr;
    return {false, "Gagal buat monitor task (heap " + String(ESP.getFreeHeap()) + "B)"};
  }

  persistAllMonitors();

  char msg[120];
  snprintf(msg, sizeof(msg),
           "Monitor pin=%d trigger=%s action=%s poll=%ums debounce=%ums once=%s",
           pin, trigStr, actionStr, pollMs, debMs, once ? "yes" : "no");
  Serial.printf("[SKILL] gpio.monitor: %s\n", msg);
  return {true, String(msg)};
}

// ─────────────────────────────────────────────────────────────────────────
// gpio.monitor_stop
// ─────────────────────────────────────────────────────────────────────────

static SkillResult gpioMonitorStopHandler(const JsonObject& args) {
  if (!args["pin"].is<int>()) {
    uint8_t stopped = 0;
    for (uint8_t i = 0; i < MAX_MONITOR_TASKS; i++) {
      if (monitorSlots[i].active) { monitorSlots[i].stopRequest = true; stopped++; }
    }
    for (int w = 0; w < 40; w++) {
      bool any = false;
      for (uint8_t i = 0; i < MAX_MONITOR_TASKS; i++)
        if (monitorSlots[i].active) { any = true; break; }
      if (!any) break;
      vTaskDelay(pdMS_TO_TICKS(50));
    }
    char msg[56];
    snprintf(msg, sizeof(msg), "Semua monitor task dihentikan (%u task)", stopped);
    Serial.printf("[SKILL] gpio.monitor_stop: %s\n", msg);
    return {stopped > 0, String(msg)};
  }
  uint8_t pin  = args["pin"].as<uint8_t>();
  uint8_t stopped = 0;
  for (uint8_t i = 0; i < MAX_MONITOR_TASKS; i++) {
    if (monitorSlots[i].active && monitorSlots[i].pin == pin) {
      monitorSlots[i].stopRequest = true; stopped++;
    }
  }
  if (stopped == 0) return {false, "Tidak ada monitor task aktif di pin " + String(pin)};
  for (int w = 0; w < 30; w++) {
    bool any = false;
    for (uint8_t i = 0; i < MAX_MONITOR_TASKS; i++)
      if (monitorSlots[i].active && monitorSlots[i].pin == pin) { any = true; break; }
    if (!any) break;
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  persistAllMonitors();

  char msg[80];
  snprintf(msg, sizeof(msg), "Monitor pin %d dihentikan (%u task)", pin, stopped);
  Serial.printf("[SKILL] gpio.monitor_stop: %s\n", msg);
  return {true, String(msg)};
}

// ─────────────────────────────────────────────────────────────────────────
// gpio.toggle
// ─────────────────────────────────────────────────────────────────────────

static SkillResult gpioToggleHandler(const JsonObject& args) {
  if (!args["pin"].is<int>())
    return {false, "Missing pin"};
  uint8_t pin  = args["pin"].as<uint8_t>();
  bool    cur  = false;
  hal::gpioSetMode(pin, hal::PinMode::PIN_OUTPUT);
  hal::gpioRead(pin, cur);
  bool val = !cur;
  String warn = "";
  if (isStrappingPin(pin)) {
    warn = " ⚠️Strapping pin!";
    Serial.printf("[WARN] gpio.toggle: GPIO %d adalah strapping pin!\n", pin);
  }
  BlinkSlot* existing = findBlinkSlotByPin(pin);
  if (existing) {
    existing->stopRequest = true;
    for (int i = 0; i < 20 && existing->active; i++) vTaskDelay(pdMS_TO_TICKS(50));
  }
  if (hal::gpioWrite(pin, val)) {
    {
      Preferences prefs;
      prefs.begin(NVS_NS, false);
      prefs.putUChar(KEY_SET, 1);
      String raw = prefs.getString(KEY_GPO, "{}");
      JsonDocument doc;
      if (raw.length() > 0) deserializeJson(doc, raw);
      doc[String(pin)] = val ? 1 : 0;
      String json; serializeJson(doc, json);
      prefs.putString(KEY_GPO, json);
      prefs.end();
    }
    Serial.printf("[SKILL] gpio.toggle pin=%d → %s\n", pin, val ? "HIGH" : "LOW");
    return {true, "GPIO " + String(pin) + " toggle → " + (val ? "HIGH (ON)" : "LOW (OFF)") + warn};
  }
  return {false, "Gagal toggle GPIO " + String(pin)};
}

// ─────────────────────────────────────────────────────────────────────────
// gpio.pulse
// ─────────────────────────────────────────────────────────────────────────

static SkillResult gpioPulseHandler(const JsonObject& args) {
  if (!args["pin"].is<int>())
    return {false, "Missing pin"};
  uint8_t pin   = args["pin"].as<uint8_t>();
  int     ms    = args["ms"]   | 200;
  bool    start = true;
  if (args["start"].is<JsonVariant>()) {
    if (args["start"].is<bool>()) start = args["start"].as<bool>();
    else start = (args["start"].as<int>() != 0);
  }
  String warn = "";
  if (isStrappingPin(pin)) {
    warn = " ⚠️Strapping pin!";
    Serial.printf("[WARN] gpio.pulse: GPIO %d adalah strapping pin!\n", pin);
  }
  BlinkSlot* existing = findBlinkSlotByPin(pin);
  if (existing) {
    existing->stopRequest = true;
    for (int i = 0; i < 20 && existing->active; i++) vTaskDelay(pdMS_TO_TICKS(50));
  }
  hal::gpioSetMode(pin, hal::PinMode::PIN_OUTPUT);
  hal::gpioWrite(pin, start);
  vTaskDelay(pdMS_TO_TICKS(ms));
  bool endVal = !start;
  hal::gpioWrite(pin, endVal);
  {
    Preferences prefs;
    prefs.begin(NVS_NS, false);
    prefs.putUChar(KEY_SET, 1);
    String raw = prefs.getString(KEY_GPO, "{}");
    JsonDocument doc;
    if (raw.length() > 0) deserializeJson(doc, raw);
    doc[String(pin)] = endVal ? 1 : 0;
    String json; serializeJson(doc, json);
    prefs.putString(KEY_GPO, json);
    prefs.end();
  }
  Serial.printf("[SKILL] gpio.pulse pin=%d ms=%d start=%s\n", pin, ms, start ? "HIGH" : "LOW");
  return {true, "GPIO " + String(pin) + " pulse " + String(ms) + "ms → " + (endVal ? "HIGH" : "LOW") + warn};
}

// ─────────────────────────────────────────────────────────────────────────
// system.restart
// ─────────────────────────────────────────────────────────────────────────

static void restartTask(void*) {
  vTaskDelay(pdMS_TO_TICKS(1500));
  Serial.println(F("[SKILL] system.restart — rebooting now"));
  ESP.restart();
  vTaskDelete(nullptr);
}

static SkillResult systemRestartHandler(const JsonObject& args) {
  Serial.println(F("[SKILL] system.restart — reboot dalam 1,5 detik"));
  xTaskCreatePinnedToCore(restartTask, "reboot", 2048, nullptr, 1, nullptr, 0);
  return {true, "ESP32 akan restart dalam 1,5 detik..."};
}

// ─────────────────────────────────────────────────────────────────────────
// system.wifi
// ─────────────────────────────────────────────────────────────────────────

static SkillResult systemWifiHandler(const JsonObject& args) {
  char msg[196];
  snprintf(msg, sizeof(msg),
           "WiFi SSID=%s RSSI=%ddBm IP=%s MAC=%s channel=%d",
           WiFi.SSID().c_str(), WiFi.RSSI(),
           WiFi.localIP().toString().c_str(),
           WiFi.macAddress().c_str(), WiFi.channel());
  Serial.printf("[SKILL] system.wifi: %s\n", msg);
  return {true, String(msg)};
}

// ─────────────────────────────────────────────────────────────────────────
// ADC Skills
// ─────────────────────────────────────────────────────────────────────────

static SkillResult adcReadHandler(const JsonObject& args) {
  if (!args["pin"].is<int>()) return {false, "Missing pin"};
  uint8_t pin = args["pin"].as<uint8_t>();
  if (pin >= 25 && pin <= 27)
    return {false, "Pin " + String(pin) + " (ADC2) tidak bisa saat WiFi aktif. Gunakan 32-39."};
  hal::AdcReading reading{};
  if (hal::adcRead(pin, reading)) {
    return {true, "ADC pin " + String(pin)
                + ": raw=" + String(reading.raw)
                + " voltage=" + String(reading.voltage, 3) + "V"};
  }
  return {false, "Gagal read ADC pin " + String(pin)};
}

// ─────────────────────────────────────────────────────────────────────────
// System Skills
// ─────────────────────────────────────────────────────────────────────────

static SkillResult systemClearPersistHandler(const JsonObject& args) {
  clearPersistentConfig();
  Serial.println(F("[SKILL] system.clear_persist: cleared, restart to apply empty config"));
  return {true, "Semua logika dihentikan. Reboot untuk apply (tanpa factory defaults)."};
}

static SkillResult systemResetFactoryHandler(const JsonObject& args) {
  resetFactoryConfig();
  Serial.println(F("[SKILL] system.reset_factory: factory reset, restart to load defaults"));
  return {true, "Factory reset. Reboot untuk kembali ke default button→LED."};
}

static SkillResult systemStatusHandler(const JsonObject& args) {
  uint8_t activeBlinks   = 0;
  uint8_t activeMonitors = 0;
  char    blinkInfo[64]   = "none";
  char    monitorInfo[80] = "none";
  int     bi = 0, mi = 0;
  bool    bf = true, mf = true;

  for (uint8_t i = 0; i < MAX_BLINK_TASKS; i++) {
    if (blinkSlots[i].active) {
      if (!bf) bi += snprintf(blinkInfo + bi, sizeof(blinkInfo) - bi, ",");
      bi += snprintf(blinkInfo + bi, sizeof(blinkInfo) - bi, "p%d", blinkSlots[i].pin);
      bf = false; activeBlinks++;
    }
  }
  for (uint8_t i = 0; i < MAX_MONITOR_TASKS; i++) {
    if (monitorSlots[i].active) {
      if (!mf) mi += snprintf(monitorInfo + mi, sizeof(monitorInfo) - mi, ",");
      mi += snprintf(monitorInfo + mi, sizeof(monitorInfo) - mi,
                     "p%d→%s", monitorSlots[i].pin, monitorSlots[i].actionTool);
      mf = false; activeMonitors++;
    }
  }

  char msg[220];
  snprintf(msg, sizeof(msg),
           "uptime=%lus heap=%uB min_heap=%uB cpu=%uMHz "
           "blinks=%u[%s] monitors=%u[%s]",
           millis() / 1000,
           ESP.getFreeHeap(), ESP.getMinFreeHeap(), ESP.getCpuFreqMHz(),
           activeBlinks,   activeBlinks   > 0 ? blinkInfo   : "none",
           activeMonitors, activeMonitors > 0 ? monitorInfo : "none");

  Serial.printf("[SKILL] system.status: %s\n", msg);
  return {true, String(msg)};
}

// ─────────────────────────────────────────────────────────────────────────
// MQTT Skills
// ─────────────────────────────────────────────────────────────────────────

static SkillResult mqttPublishHandler(const JsonObject& args) {
  if (!args["topic"].is<const char*>())
    return {false, "Missing topic"};
  if (!args["payload"].is<const char*>())
    return {false, "Missing payload"};

  const char* topic   = args["topic"];
  const char* payload = args["payload"];

  if (mqttPublishFn) {
    mqttPublishFn(topic, payload);
    return {true, "MQTT publish: " + String(topic)};
  }
  return {false, "MQTT client not available"};
}

static SkillResult mqttSubscribeHandler(const JsonObject& args) {
  if (!args["topic"].is<const char*>())
    return {false, "Missing topic"};
  if (!args["action"].is<const char*>())
    return {false, "Missing action"};

  const char* topic  = args["topic"];
  const char* action = args["action"];

  if (!hasSkill(action))
    return {false, "Action skill tidak ditemukan: " + String(action)};

  char actionArgs[128] = "{}";
  if (args["action_args"].is<JsonObject>())
    serializeJson(args["action_args"], actionArgs, sizeof(actionArgs));

  if (mqttAddSub(topic, action, actionArgs)) {
    persistMqttSubs();
    return {true, "MQTT subscribed: " + String(topic) + " → " + String(action)};
  }
  return {false, "MQTT subscription penuh (max " + String(MAX_MQTT_SUBS) + ")"};
}

static SkillResult mqttStatusHandler(const JsonObject& args) {
  uint8_t subs = mqttActiveSubCount();
  char msg[96];
  snprintf(msg, sizeof(msg), "MQTT: %s | %u subscription(s) | broker: %s",
           subs > 0 ? "configured" : "no subs",
           subs,
           subs > 0 ? listMqttSubs().c_str() : "—");
  return {true, String(msg)};
}

static SkillResult mqttReconnectHandler(const JsonObject& args) {
  Serial.println(F("[SKILL] mqtt.reconnect — requesting reconnect"));
  if (mqttReconnectFn) mqttReconnectFn();
  return {true, "MQTT reconnect requested"};
}

static SkillResult mqttListSubsHandler(const JsonObject& args) {
  uint8_t count = mqttActiveSubCount();
  if (count == 0)
    return {true, "Tidak ada MQTT subscription aktif"};
  String detail = listMqttSubs();
  char msg[160];
  snprintf(msg, sizeof(msg), "MQTT subscriptions (%u): %s", count, detail.c_str());
  return {true, String(msg)};
}

static SkillResult mqttUnsubscribeHandler(const JsonObject& args) {
  const char* topic = args["topic"] | "";

  bool ok;
  if (strlen(topic) == 0) {
    ok = mqttRemoveSub(nullptr);
  } else {
    ok = mqttRemoveSub(topic);
  }

  if (ok) {
    persistMqttSubs();
    if (strlen(topic) == 0)
      return {true, "Semua MQTT subscription dihapus"};
    else
      return {true, "MQTT unsubscribed: " + String(topic)};
  }
  return {false, "Tidak ada subscription aktif" + String(strlen(topic) > 0 ? " untuk " + String(topic) : "")};
}

// ─────────────────────────────────────────────────────────────────────────
// mqtt.gpio_bridge — subscribe topic, parse int, >0 → HIGH, 0 → LOW
// ─────────────────────────────────────────────────────────────────────────

static SkillResult mqttGpioBridgeHandler(const JsonObject& args) {
  if (!args["topic"].is<const char*>())
    return {false, "Missing topic"};
  if (!args["pin"].is<int>())
    return {false, "Missing pin"};

  const char* topic = args["topic"];
  uint8_t pin = args["pin"].as<uint8_t>();

  // Build action_args: {"pin":pin,"value":"{PAYLOAD}"}
  char actionArgs[96];
  snprintf(actionArgs, sizeof(actionArgs),
           "{\"pin\":%u,\"value\":\"{PAYLOAD}\"}", pin);

  // Use mqttAddSub with action="gpio.write" and the payload template
  if (mqttAddSub(topic, "gpio.write", actionArgs)) {
    persistMqttSubs();
    char msg[128];
    snprintf(msg, sizeof(msg),
             "MQTT bridge topic=%s → GPIO %u (>0=ON,0=OFF)", topic, pin);
    Serial.printf("[SKILL] mqtt.gpio_bridge: %s\n", msg);
    return {true, String(msg)};
  }
  return {false, "Gagal subscribe MQTT (slot penuh)"};
}

// ─────────────────────────────────────────────────────────────────────────
// gpio.write_pwm
// ─────────────────────────────────────────────────────────────────────────

static SkillResult gpioWritePwmHandler(const JsonObject& args) {
  if (!args["pin"].is<int>() || !args["duty"].is<int>())
    return {false, "Missing pin or duty"};
  uint8_t pin = args["pin"].as<uint8_t>();
  int duty = args["duty"].as<int>();
  int freq = args["freq"] | 5000;
  if (duty < 0) duty = 0;
  if (duty > 255) duty = 255;
  if (freq < 1) freq = 1;

  int8_t idx = pwmFindByPin(pin);
  if (idx >= 0) {
    ledcWrite(pwmSlots[idx].channel, duty);
    pwmSlots[idx].duty = (uint8_t)duty;
  } else {
    int8_t ch = pwmAllocChannel(pin);
    if (ch < 0) return {false, "PWM channel penuh (max 8)"};
    ledcSetup((uint8_t)ch, freq, 8);
    ledcAttachPin(pin, (uint8_t)ch);
    ledcWrite((uint8_t)ch, duty);
    pwmSlots[(uint8_t)ch] = {true, pin, (uint8_t)ch, (uint32_t)freq, (uint8_t)duty};
  }

  // Persist PWM config
  {
    Preferences prefs;
    prefs.begin(NVS_NS, false);
    prefs.putUChar(KEY_SET, 1);
    String raw = prefs.getString(KEY_PWM, "{}");
    prefs.end();
    JsonDocument doc;
    if (raw.length() > 0) deserializeJson(doc, raw);
    doc[String(pin)] = duty == 0 ? 0 : freq * (duty > 0 ? 1 : -1);
    String json; serializeJson(doc, json);
    prefs.begin(NVS_NS, false);
    prefs.putString(KEY_PWM, json);
    prefs.end();
  }

  char buf[64];
  snprintf(buf, sizeof(buf), "PWM P%d = %d%% (freq %d Hz)", pin, duty * 100 / 255, freq);
  return {true, buf};
}

// ─────────────────────────────────────────────────────────────────────────
// gpio.servo
// ─────────────────────────────────────────────────────────────────────────

static SkillResult gpioServoHandler(const JsonObject& args) {
  if (!args["pin"].is<int>() || !args["angle"].is<int>())
    return {false, "Missing pin or angle"};
  uint8_t pin = args["pin"].as<uint8_t>();
  int angle = args["angle"].as<int>();
  if (angle < 0) angle = 0;
  if (angle > 180) angle = 180;

  int8_t idx = servoFindByPin(pin);
  if (idx >= 0) {
    uint32_t duty = servoAngleToDuty(angle);
    ledcWrite(servoSlots[idx].channel, duty);
  } else {
    idx = servoFindFree();
    if (idx < 0) return {false, "Servo slot penuh (max 4)"};
    uint8_t ch = SERVO_BASE_CH + idx;
    ledcSetup(ch, 50, 12);
    ledcAttachPin(pin, ch);
    uint32_t duty = servoAngleToDuty(angle);
    ledcWrite(ch, duty);
    servoSlots[idx] = {true, pin, ch};
  }

  char buf[48];
  snprintf(buf, sizeof(buf), "Servo P%d = %d°", pin, angle);
  return {true, buf};
}

// ─────────────────────────────────────────────────────────────────────────
// gpio.debounce
// ─────────────────────────────────────────────────────────────────────────

static SkillResult gpioDebounceHandler(const JsonObject& args) {
  if (!args["pin"].is<int>())
    return {false, "Missing pin"};
  if (!args["action"].is<const char*>())
    return {false, "Missing action"};
  uint8_t pin = args["pin"].as<uint8_t>();
  uint32_t ms = args["ms"] | 50;
  const char* action = args["action"];
  char actionArgs[128] = "{}";
  if (args["action_args"].is<JsonObject>())
    serializeJson(args["action_args"], actionArgs, sizeof(actionArgs));

  DebounceSlot* existing = debounceFindByPin(pin);
  if (existing) {
    existing->stopRequest = true;
    vTaskDelay(pdMS_TO_TICKS(20));
  }

  DebounceSlot* slot = debounceFindFree();
  if (!slot) return {false, "Debounce slot penuh (max 4)"};

  slot->active      = true;
  slot->stopRequest = false;
  slot->pin         = pin;
  slot->ms          = ms;
  strncpy(slot->actionTool, action, sizeof(slot->actionTool) - 1);
  slot->actionTool[sizeof(slot->actionTool) - 1] = '\0';
  strncpy(slot->actionArgs, actionArgs, sizeof(slot->actionArgs) - 1);
  slot->actionArgs[sizeof(slot->actionArgs) - 1] = '\0';

  char taskName[16];
  snprintf(taskName, sizeof(taskName), "dbP%d", pin);
  xTaskCreatePinnedToCore(debounceTaskFn, taskName, 2048, (void*)slot, 1, &slot->handle, 0);

  // Persist
  {
    Preferences prefs;
    prefs.begin(NVS_NS, false);
    prefs.putUChar(KEY_SET, 1);
    String raw = prefs.getString(KEY_DBN, "[]");
    prefs.end();
    JsonDocument doc;
    if (raw.length() > 2) deserializeJson(doc, raw);
    JsonArray a = doc.as<JsonArray>();
    bool found = false;
    for (JsonObject o : a) {
      if (o["pin"].as<uint8_t>() == pin) {
        o["ms"] = ms; o["action"] = action; found = true; break;
      }
    }
    if (!found) {
      JsonObject o = a.add<JsonObject>();
      o["pin"] = pin; o["ms"] = ms; o["action"] = action;
    }
    String json; serializeJson(doc, json);
    prefs.begin(NVS_NS, false);
    prefs.putString(KEY_DBN, json);
    prefs.end();
  }

  char buf[64];
  snprintf(buf, sizeof(buf), "Debounce P%d (%ums) → %s", pin, ms, action);
  return {true, buf};
}

// ─────────────────────────────────────────────────────────────────────────
// system.memory
// ─────────────────────────────────────────────────────────────────────────

static SkillResult systemMemoryHandler(const JsonObject& args) {
  char msg[128];
  snprintf(msg, sizeof(msg),
           "Heap: free=%uB min=%uB max=%uB PSRAM=%s",
           ESP.getFreeHeap(), ESP.getMinFreeHeap(), ESP.getHeapSize(),
           ESP.getPsramSize() > 0 ? String(ESP.getFreePsram()) + "B free" : "none");
  return {true, String(msg)};
}

// ─────────────────────────────────────────────────────────────────────────
// mqtt.publish_on_change
// ─────────────────────────────────────────────────────────────────────────

static SkillResult mqttPublishOnChangeHandler(const JsonObject& args) {
  if (!args["pin"].is<int>() || !args["topic"].is<const char*>())
    return {false, "Missing pin or topic"};
  uint8_t pin = args["pin"].as<uint8_t>();
  const char* topic = args["topic"];
  const char* highMsg = args["high_msg"] | "1";
  const char* lowMsg  = args["low_msg"]  | "0";

  if (!mqttPublishFn)
    return {false, "MQTT client not available"};

  PubChangeSlot* existing = pubChangeFindByPin(pin);
  if (existing) {
    existing->stopRequest = true;
    vTaskDelay(pdMS_TO_TICKS(20));
  }

  PubChangeSlot* slot = pubChangeFindFree();
  if (!slot) return {false, "Publish-on-change slot penuh (max 4)"};

  slot->active      = true;
  slot->stopRequest = false;
  slot->pin         = pin;
  strncpy(slot->topic,  topic,  sizeof(slot->topic)  - 1);
  strncpy(slot->highMsg, highMsg, sizeof(slot->highMsg) - 1);
  strncpy(slot->lowMsg,  lowMsg,  sizeof(slot->lowMsg)  - 1);

  char taskName[16];
  snprintf(taskName, sizeof(taskName), "pcP%d", pin);
  xTaskCreatePinnedToCore(pubChangeTaskFn, taskName, 2048, (void*)slot, 1, &slot->handle, 0);

  // Persist
  {
    Preferences prefs;
    prefs.begin(NVS_NS, false);
    prefs.putUChar(KEY_SET, 1);
    String raw = prefs.getString(KEY_POC, "[]");
    prefs.end();
    JsonDocument doc;
    if (raw.length() > 2) deserializeJson(doc, raw);
    JsonArray a = doc.as<JsonArray>();
    bool found = false;
    for (JsonObject o : a) {
      if (o["pin"].as<uint8_t>() == pin) {
        o["topic"] = topic; o["high"] = highMsg; o["low"] = lowMsg; found = true; break;
      }
    }
    if (!found) {
      JsonObject o = a.add<JsonObject>();
      o["pin"] = pin; o["topic"] = topic; o["high"] = highMsg; o["low"] = lowMsg;
    }
    String json; serializeJson(doc, json);
    prefs.begin(NVS_NS, false);
    prefs.putString(KEY_POC, json);
    prefs.end();
  }

  char buf[96];
  snprintf(buf, sizeof(buf), "Publish-on-change P%d → %s (HIGH=%s, LOW=%s)", pin, topic, highMsg, lowMsg);
  return {true, buf};
}

// ─────────────────────────────────────────────────────────────────────────
// system.sleep
// ─────────────────────────────────────────────────────────────────────────

static SkillResult systemSleepHandler(const JsonObject& args) {
  if (!args["seconds"].is<int>())
    return {false, "Missing seconds"};
  int seconds = args["seconds"].as<int>();
  if (seconds < 1) seconds = 1;

  int wakePin = args["wake_pin"] | -1;

  Serial.printf("[SKILL] system.sleep %ds wake_pin=%d\n", seconds, wakePin);

  // Respond dulu via Serial, lalu sleep
  char msg[64];
  snprintf(msg, sizeof(msg), "ESP32 deep sleep %ds", seconds);

  // Send response before sleeping
  Serial.printf("[SLEEP] %s\n", msg);

  // Configure wake-up sources
  esp_sleep_enable_timer_wakeup((uint64_t)seconds * 1000000ULL);
  if (wakePin >= 0) {
    gpio_set_pull_mode((gpio_num_t)wakePin, GPIO_PULLUP_ONLY);
    esp_sleep_enable_ext0_wakeup((gpio_num_t)wakePin, 0);
  }

  // Small delay so response can be sent via WiFi
  delay(200);

  esp_deep_sleep_start();
  return {true, String(msg)};  // never reached
}

// ─────────────────────────────────────────────────────────────────────────
// file.write / file.read
// ─────────────────────────────────────────────────────────────────────────

static SkillResult fileWriteHandler(const JsonObject& args) {
  if (!args["path"].is<const char*>() || !args["content"].is<const char*>())
    return {false, "Missing path or content"};
  const char* path = args["path"];
  const char* content = args["content"];

  // Store in NVS as JSON map under KEY_FIL
  {
    Preferences prefs;
    prefs.begin(NVS_NS, false);
    String raw = prefs.getString(KEY_FIL, "{}");
    JsonDocument doc;
    if (raw.length() > 2) deserializeJson(doc, raw);
    doc[path] = content;
    String json; serializeJson(doc, json);
    prefs.putString(KEY_FIL, json);
    prefs.end();
  }

  return {true, "File saved: " + String(path) + " (" + String(strlen(content)) + " bytes via NVS)"};
}

static SkillResult fileReadHandler(const JsonObject& args) {
  if (!args["path"].is<const char*>())
    return {false, "Missing path"};
  const char* path = args["path"];

  Preferences prefs;
  prefs.begin(NVS_NS, true);
  String raw = prefs.getString(KEY_FIL, "{}");
  prefs.end();
  JsonDocument doc;
  if (raw.length() > 2 && deserializeJson(doc, raw) == DeserializationError::Ok) {
    if (doc[path].is<const char*>()) {
      String content = doc[path].as<String>();
      size_t len = content.length();
      if (len > 500) content = content.substring(0, 500) + "... (truncated)";
      return {true, "File " + String(path) + ": " + content};
    }
  }
  return {false, "File not found: " + String(path)};
}

void mqttForEachActiveSub(void (*fn)(const char* topic)) {
  for (uint8_t i = 0; i < MAX_MQTT_SUBS; i++) {
    if (mqttSubSlots[i].active)
      fn(mqttSubSlots[i].topic);
  }
}

void mqttMarkAllUnsynced() {
  for (uint8_t i = 0; i < MAX_MQTT_SUBS; i++)
    mqttSubSlots[i].brokerSynced = false;
}

void mqttSyncSubs(bool (*subFn)(const char* topic), bool (*unsubFn)(const char* topic)) {
  for (uint8_t i = 0; i < MAX_MQTT_SUBS; i++) {
    if (mqttSubSlots[i].active && !mqttSubSlots[i].brokerSynced) {
      if (subFn(mqttSubSlots[i].topic)) {
        mqttSubSlots[i].brokerSynced = true;
        Serial.printf("[MQTT] Synced sub: %s\n", mqttSubSlots[i].topic);
      }
    }
    if (!mqttSubSlots[i].active && mqttSubSlots[i].brokerSynced) {
      if (unsubFn(mqttSubSlots[i].topic)) {
        mqttSubSlots[i].brokerSynced = false;
        Serial.printf("[MQTT] Synced unsub: %s\n", mqttSubSlots[i].topic);
      }
    }
  }
}

bool mqttAddSub(const char* topic, const char* action, const char* actionArgs) {
  // Check if already exists — update
  for (uint8_t i = 0; i < MAX_MQTT_SUBS; i++) {
    if (mqttSubSlots[i].active && strcmp(mqttSubSlots[i].topic, topic) == 0) {
      mqttSubSlots[i].brokerSynced = false;
      strncpy(mqttSubSlots[i].actionTool, action, sizeof(mqttSubSlots[i].actionTool) - 1);
      mqttSubSlots[i].actionTool[sizeof(mqttSubSlots[i].actionTool) - 1] = '\0';
      strncpy(mqttSubSlots[i].actionArgs, actionArgs, sizeof(mqttSubSlots[i].actionArgs) - 1);
      mqttSubSlots[i].actionArgs[sizeof(mqttSubSlots[i].actionArgs) - 1] = '\0';
      return true;
    }
  }
  // Find free slot
  for (uint8_t i = 0; i < MAX_MQTT_SUBS; i++) {
    if (!mqttSubSlots[i].active) {
      mqttSubSlots[i].active       = true;
      mqttSubSlots[i].brokerSynced = false;
      strncpy(mqttSubSlots[i].topic, topic, sizeof(mqttSubSlots[i].topic) - 1);
      mqttSubSlots[i].topic[sizeof(mqttSubSlots[i].topic) - 1] = '\0';
      strncpy(mqttSubSlots[i].actionTool, action, sizeof(mqttSubSlots[i].actionTool) - 1);
      mqttSubSlots[i].actionTool[sizeof(mqttSubSlots[i].actionTool) - 1] = '\0';
      strncpy(mqttSubSlots[i].actionArgs, actionArgs, sizeof(mqttSubSlots[i].actionArgs) - 1);
      mqttSubSlots[i].actionArgs[sizeof(mqttSubSlots[i].actionArgs) - 1] = '\0';
      return true;
    }
  }
  return false;
}

bool mqttRemoveSub(const char* topic) {
  if (!topic || strlen(topic) == 0) {
    bool any = false;
    for (uint8_t i = 0; i < MAX_MQTT_SUBS; i++) {
      if (mqttSubSlots[i].active) {
        mqttSubSlots[i].active       = false;
        mqttSubSlots[i].brokerSynced = false;
        any = true;
      }
    }
    return any;
  }
  for (uint8_t i = 0; i < MAX_MQTT_SUBS; i++) {
    if (mqttSubSlots[i].active && strcmp(mqttSubSlots[i].topic, topic) == 0) {
      mqttSubSlots[i].active       = false;
      mqttSubSlots[i].brokerSynced = false;
      return true;
    }
  }
  return false;
}

void mqttOnMessage(const char* topic, const char* payload) {
  Serial.printf("[MQTT] Message: %s → %s\n", topic, payload);

  // Built-in: topic "hello/10219201/test" → GPIO 27 on/off
  if (strcmp(topic, "hello/10219201/test") == 0) {
    bool isOn = (strcmp(payload, "on") == 0);
    if (isOn || strcmp(payload, "off") == 0) {
      pinMode(27, OUTPUT);
      digitalWrite(27, isOn ? HIGH : LOW);
      Serial.printf("[MQTT] GPIO 27 → %s\n", isOn ? "ON" : "OFF");
    }
    return;
  }

  for (uint8_t i = 0; i < MAX_MQTT_SUBS; i++) {
    if (!mqttSubSlots[i].active) continue;

    if (topicMatches(mqttSubSlots[i].topic, topic)) {
      Serial.printf("[MQTT] Match sub=%s dispatching action=%s\n",
                    mqttSubSlots[i].topic, mqttSubSlots[i].actionTool);

      if (s_dispatch != nullptr) {
        // Ganti {PAYLOAD} dengan payload asli
        char resolvedArgs[128];
        strncpy(resolvedArgs, mqttSubSlots[i].actionArgs, sizeof(resolvedArgs) - 1);
        const char* placeholder = strstr(resolvedArgs, "{PAYLOAD}");
        if (placeholder != nullptr) {
          String before = String(resolvedArgs).substring(0, placeholder - resolvedArgs);
          String after  = String(resolvedArgs).substring(
                           (placeholder - resolvedArgs) + strlen("{PAYLOAD}"));
          String escaped = String(payload);
          // Escape special JSON chars in payload sebelum disisipkan
          escaped.replace("\\", "\\\\");
          escaped.replace("\"", "\\\"");
          String replaced = before + escaped + after;
          strncpy(resolvedArgs, replaced.c_str(), sizeof(resolvedArgs) - 1);
          Serial.printf("[MQTT] Substituted {PAYLOAD}: %s\n", resolvedArgs);
        }

        SkillResponse resp;
        memset(&resp, 0, sizeof(resp));
        s_dispatch(mqttSubSlots[i].actionTool, resolvedArgs,
                   resp, pdMS_TO_TICKS(5000));
        Serial.printf("[MQTT] Action %s → %s: %s\n",
                      mqttSubSlots[i].actionTool,
                      resp.success ? "OK" : "FAIL", resp.message);
      } else {
        Serial.println(F("[MQTT] WARN: dispatcher belum di-set!"));
      }
    }
  }
}

uint8_t mqttActiveSubCount() {
  uint8_t c = 0;
  for (uint8_t i = 0; i < MAX_MQTT_SUBS; i++)
    if (mqttSubSlots[i].active) c++;
  return c;
}

String listMqttSubs() {
  String list = "";
  for (uint8_t i = 0; i < MAX_MQTT_SUBS; i++) {
    if (!mqttSubSlots[i].active) continue;
    if (list.length() > 0) list += ", ";
    list += String(mqttSubSlots[i].topic) + "→" + String(mqttSubSlots[i].actionTool);
  }
  return list;
}

String mqttSubsJson() {
  String json = "[";
  for (uint8_t i = 0; i < MAX_MQTT_SUBS; i++) {
    if (!mqttSubSlots[i].active) continue;
    if (json.length() > 1) json += ",";
    json += "{\"topic\":\"" + String(mqttSubSlots[i].topic) + "\"";
    json += ",\"tool\":\"" + String(mqttSubSlots[i].actionTool) + "\"";
    json += ",\"args\":" + String(mqttSubSlots[i].actionArgs) + "}";
  }
  json += "]";
  return json;
}

// ─────────────────────────────────────────────────────────────────────────
// Registry API
// ─────────────────────────────────────────────────────────────────────────

void registerSkill(const char* name, SkillHandler handler) {
  if (regCount >= MAX_SKILLS) { Serial.println(F("[SKILL] WARN: Registry penuh!")); return; }
  strncpy(registry[regCount].name, name, sizeof(registry[0].name) - 1);
  registry[regCount].handler = handler;
  regCount++;
}

bool hasSkill(const char* name) {
  for (uint8_t i = 0; i < regCount; i++)
    if (strcmp(registry[i].name, name) == 0) return true;
  return false;
}

uint8_t skillCount() { return regCount; }

String listSkills() {
  String list = "";
  for (uint8_t i = 0; i < regCount; i++) {
    if (i > 0) list += ", ";
    list += registry[i].name;
  }
  return list;
}

// ─────────────────────────────────────────────────────────────────────────
// Execution
// ─────────────────────────────────────────────────────────────────────────

SkillResult executeSkillByName(const String& name, const JsonObject& args) {
  for (uint8_t i = 0; i < regCount; i++)
    if (name == registry[i].name) return registry[i].handler(args);
  Serial.printf("[SKILL] Unknown: %s  Available: %s\n",
                name.c_str(), listSkills().c_str());
  return {false, "Skill tidak ditemukan: " + name};
}

SkillResult executeSkill(const JsonObject& skillJson) {
  // ArduinoJson v7: ganti containsKey("tool") dengan is<const char*>()
  if (!skillJson["tool"].is<const char*>()) return {false, "Missing 'tool' field"};
  const char* toolName = skillJson["tool"] | "";
  if (skillJson["args"].is<JsonObject>()) {
    return executeSkillByName(String(toolName), skillJson["args"].as<JsonObject>());
  } else {
    // ArduinoJson v7: ganti StaticJsonDocument<16> dengan JsonDocument
    JsonDocument emptyDoc;
    JsonObject   emptyArgs = emptyDoc.to<JsonObject>();
    return executeSkillByName(String(toolName), emptyArgs);
  }
}

bool executeSkillChain(const JsonArray& skillArray, JsonArray& results) {
  bool allSuccess = true;
  for (JsonObject skillObj : skillArray) {
    SkillResult result = executeSkill(skillObj);
    JsonObject  resObj = results.add<JsonObject>();
    resObj["tool"]    = skillObj["tool"] | "unknown";
    resObj["success"] = result.success;
    resObj["message"] = result.message;
    if (!result.success) allSuccess = false;
  }
  return allSuccess;
}

// ─────────────────────────────────────────────────────────────────────────
// NVS Persistent Config
// ─────────────────────────────────────────────────────────────────────────

static void loadPwmConfig();
static void loadDebounceConfig();
static void loadPubChangeConfig();

static void persistGpioMode(uint8_t pin, const char* mode) {
  Preferences prefs;
  prefs.begin(NVS_NS, false);
  prefs.putUChar(KEY_SET, 1);
  String raw = prefs.getString(KEY_GIO, "{}");
  JsonDocument doc;
  if (raw.length() > 0) deserializeJson(doc, raw);
  doc[String(pin)] = mode;
  String json; serializeJson(doc, json);
  prefs.putString(KEY_GIO, json);
  prefs.end();
}

static void persistAllMonitors() {
  Preferences prefs;
  prefs.begin(NVS_NS, false);
  prefs.putUChar(KEY_SET, 1);
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (uint8_t i = 0; i < MAX_MONITOR_TASKS; i++) {
    if (!monitorSlots[i].active) continue;
    JsonObject m = arr.add<JsonObject>();
    m["pin"] = monitorSlots[i].pin;
    const char* t = "low";
    if      (monitorSlots[i].trigger == MonitorTrigger::TRIG_HIGH)   t = "high";
    else if (monitorSlots[i].trigger == MonitorTrigger::TRIG_CHANGE) t = "change";
    m["trigger"]   = t;
    m["action"]    = monitorSlots[i].actionTool;
    m["poll"]      = monitorSlots[i].pollMs;
    m["deb"]       = monitorSlots[i].debounceMs;
    m["once"]      = monitorSlots[i].once;
    if (strlen(monitorSlots[i].actionArgs) > 2) {
      JsonDocument ad;
      if (deserializeJson(ad, monitorSlots[i].actionArgs) == DeserializationError::Ok)
        m["args"] = ad.as<JsonObject>();
    }
  }
  String json;
  serializeJson(arr, json);
  prefs.putString(KEY_MON, json);
  prefs.end();
}

// ─────────────────────────────────────────────────────────────────────────
// MQTT subscription persistence
// ─────────────────────────────────────────────────────────────────────────

void persistMqttSubs() {
  Preferences prefs;
  prefs.begin(NVS_NS, false);
  prefs.putUChar(KEY_SET, 1);
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (uint8_t i = 0; i < MAX_MQTT_SUBS; i++) {
    if (!mqttSubSlots[i].active) continue;
    JsonObject m = arr.add<JsonObject>();
    m["topic"]  = mqttSubSlots[i].topic;
    m["action"] = mqttSubSlots[i].actionTool;
    if (strlen(mqttSubSlots[i].actionArgs) > 2) {
      JsonDocument ad;
      if (deserializeJson(ad, mqttSubSlots[i].actionArgs) == DeserializationError::Ok)
        m["args"] = ad.as<JsonObject>();
    }
  }
  String json;
  serializeJson(arr, json);
  prefs.putString(KEY_MQT, json);
  prefs.end();
}

// ─────────────────────────────────────────────────────────────────────────
// Init
// ─────────────────────────────────────────────────────────────────────────

void init() {
  if (_initialized) return;
  initBlinkSlots();
  initMonitorSlots();
  initMqttSubSlots();
  initPwmSlots();
  initServoSlots();
  initDebounceSlots();
  initPubChangeSlots();
  regCount = 0;

  registerSkill("gpio.write",        gpioWriteHandler);
  registerSkill("gpio.read",         gpioReadHandler);
  registerSkill("gpio.toggle",       gpioToggleHandler);
  registerSkill("gpio.pulse",        gpioPulseHandler);
  registerSkill("gpio.mode",         gpioModeHandler);
  registerSkill("gpio.blink",        gpioBlinkHandler);
  registerSkill("gpio.blink_start",  gpioBlinkStartHandler);
  registerSkill("gpio.blink_stop",   gpioBlinkStopHandler);
  registerSkill("gpio.monitor",      gpioMonitorHandler);
  registerSkill("gpio.monitor_stop", gpioMonitorStopHandler);
  registerSkill("adc.read",          adcReadHandler);
  registerSkill("system.status",        systemStatusHandler);
  registerSkill("system.restart",       systemRestartHandler);
  registerSkill("system.wifi",          systemWifiHandler);
  registerSkill("system.clear_persist",  systemClearPersistHandler);
  registerSkill("system.reset_factory",  systemResetFactoryHandler);
  registerSkill("mqtt.publish",       mqttPublishHandler);
  registerSkill("mqtt.subscribe",     mqttSubscribeHandler);
  registerSkill("mqtt.unsubscribe",   mqttUnsubscribeHandler);
  registerSkill("mqtt.gpio_bridge",   mqttGpioBridgeHandler);
  registerSkill("mqtt.status",        mqttStatusHandler);
  registerSkill("mqtt.reconnect",     mqttReconnectHandler);
  registerSkill("mqtt.listsubs",      mqttListSubsHandler);
  registerSkill("gpio.write_pwm",     gpioWritePwmHandler);
  registerSkill("gpio.servo",         gpioServoHandler);
  registerSkill("gpio.debounce",      gpioDebounceHandler);
  registerSkill("system.memory",      systemMemoryHandler);
  registerSkill("system.sleep",       systemSleepHandler);
  registerSkill("mqtt.publish_on_change", mqttPublishOnChangeHandler);
  registerSkill("file.write",         fileWriteHandler);
  registerSkill("file.read",          fileReadHandler);

  Serial.printf("[SKILL] Registered %d skills: %s\n",
                regCount, listSkills().c_str());
  _initialized = true;
}

bool isInitialized() { return _initialized; }

// ─────────────────────────────────────────────────────────────────────────
// Persistent config — load dari NVS
// ─────────────────────────────────────────────────────────────────────────

uint8_t loadPersistentConfig() {
  uint8_t restored = 0;

  // 1. Restore GPIO modes
  {
    Preferences prefs;
    prefs.begin(NVS_NS, true);
    String raw = prefs.getString(KEY_GIO, "{}");
    prefs.end();
    JsonDocument doc;
    if (raw.length() > 0 && deserializeJson(doc, raw) == DeserializationError::Ok) {
      JsonObject cfg = doc.as<JsonObject>();
      for (JsonPair kv : cfg) {
        uint8_t pin = (uint8_t)atoi(kv.key().c_str());
        const char* mode = kv.value().as<const char*>();
        if (!mode) continue;
        JsonDocument a;
        a["pin"] = pin; a["mode"] = mode;
        gpioModeHandler(a.as<JsonObject>());
        Serial.printf("[NVS] Restore GPIO pin=%d mode=%s\n", pin, mode);
      }
    }
  }

  // 1.5 Restore GPIO output values (must be after mode restore)
  {
    Preferences prefs;
    prefs.begin(NVS_NS, true);
    String raw = prefs.getString(KEY_GPO, "{}");
    prefs.end();
    JsonDocument doc;
    if (raw.length() > 0 && deserializeJson(doc, raw) == DeserializationError::Ok) {
      JsonObject cfg = doc.as<JsonObject>();
      for (JsonPair kv : cfg) {
        uint8_t pin = (uint8_t)atoi(kv.key().c_str());
        int val = kv.value().as<int>();
        hal::gpioSetMode(pin, hal::PinMode::PIN_OUTPUT);
        hal::gpioWrite(pin, val != 0);
        Serial.printf("[NVS] Restore GPIO pin=%d value=%s\n", pin, val ? "HIGH" : "LOW");
        restored++;
      }
    }
  }

  // 1.6 Restore blink tasks
  {
    Preferences prefs;
    prefs.begin(NVS_NS, true);
    String raw = prefs.getString(KEY_BLK, "[]");
    prefs.end();
    JsonDocument doc;
    if (raw.length() > 2 && deserializeJson(doc, raw) == DeserializationError::Ok) {
      JsonArray arr = doc.as<JsonArray>();
      for (JsonObject b : arr) {
        uint8_t pin   = b["pin"].as<uint8_t>();
        int on_ms     = b["on_ms"]  | 500;
        int off_ms    = b["off_ms"] | 500;
        JsonDocument a;
        a["pin"] = pin; a["on_ms"] = on_ms; a["off_ms"] = off_ms;
        gpioBlinkStartHandler(a.as<JsonObject>());
        Serial.printf("[NVS] Restore blink pin=%d on=%ums off=%ums\n", pin, on_ms, off_ms);
        restored++;
      }
    }
  }

  // 2. Restore monitors
  {
    Preferences prefs;
    prefs.begin(NVS_NS, true);
    String raw = prefs.getString(KEY_MON, "[]");
    prefs.end();
    JsonDocument doc;
    if (raw.length() <= 2) return restored;
    if (deserializeJson(doc, raw) != DeserializationError::Ok) return restored;
    JsonArray arr = doc.as<JsonArray>();
    for (JsonObject m : arr) {
      JsonDocument a;
      a["pin"]         = m["pin"].as<uint8_t>();
      a["trigger"]     = m["trigger"].as<const char*>();
      a["action"]      = m["action"].as<const char*>();
      a["poll_ms"]     = m["poll"].as<uint32_t>();
      a["debounce_ms"] = m["deb"].as<uint32_t>();
      a["once"]        = m["once"].as<bool>();
      if (m["args"].is<JsonObject>())
        a["action_args"] = m["args"].as<JsonObject>();
      gpioMonitorHandler(a.as<JsonObject>());
      restored++;
    }
  }

  // 3. Restore MQTT subscriptions
  {
    Preferences prefs;
    prefs.begin(NVS_NS, true);
    String raw = prefs.getString(KEY_MQT, "[]");
    prefs.end();
    JsonDocument doc;
    if (raw.length() > 2 && deserializeJson(doc, raw) == DeserializationError::Ok) {
      JsonArray arr = doc.as<JsonArray>();
      for (JsonObject m : arr) {
        const char* topic  = m["topic"] | "";
        const char* action = m["action"] | "";
        if (strlen(topic) == 0 || strlen(action) == 0) continue;
        char actionArgs[128] = "{}";
        if (m["args"].is<JsonObject>())
          serializeJson(m["args"], actionArgs, sizeof(actionArgs));
        mqttAddSub(topic, action, actionArgs);
        Serial.printf("[NVS] Restore MQTT sub: %s → %s\n", topic, action);
        restored++;
      }
    }
  }

  // 4. Restore PWM, debounce, publish-on-change
  loadPwmConfig();
  loadDebounceConfig();
  loadPubChangeConfig();

  Serial.printf("[NVS] Restored %u items (monitors+gpio+mqtt)\n", restored);
  return restored;
}

static void loadPwmConfig() {
  Preferences prefs;
  prefs.begin(NVS_NS, true);
  String raw = prefs.getString(KEY_PWM, "{}");
  prefs.end();
  JsonDocument doc;
  if (raw.length() > 2 && deserializeJson(doc, raw) == DeserializationError::Ok) {
    for (JsonPair kv : doc.as<JsonObject>()) {
      uint8_t pin = (uint8_t)atoi(kv.key().c_str());
      int val = kv.value().as<int>();
      if (val == 0) continue;
      uint32_t freq = abs(val);
      uint8_t duty  = val > 0 ? 128 : 64; // >0 = 50%, <0 = 25%
      int8_t ch = pwmAllocChannel(pin);
      if (ch < 0) continue;
      ledcSetup((uint8_t)ch, freq, 8);
      ledcAttachPin(pin, (uint8_t)ch);
      ledcWrite((uint8_t)ch, duty);
      pwmSlots[(uint8_t)ch] = {true, pin, (uint8_t)ch, freq, duty};
    }
  }
}

static void loadDebounceConfig() {
  Preferences prefs;
  prefs.begin(NVS_NS, true);
  String raw = prefs.getString(KEY_DBN, "[]");
  prefs.end();
  JsonDocument doc;
  if (raw.length() > 2 && deserializeJson(doc, raw) == DeserializationError::Ok) {
    for (JsonObject o : doc.as<JsonArray>()) {
      uint8_t pin = o["pin"].as<uint8_t>();
      uint32_t ms = o["ms"] | 50;
      const char* action = o["action"] | "";
      if (strlen(action) == 0) continue;
      JsonDocument a;
      a["pin"] = pin; a["ms"] = ms; a["action"] = action;
      if (o["args"].is<JsonObject>())
        a["action_args"] = o["args"].as<JsonObject>();
      gpioDebounceHandler(a.as<JsonObject>());
    }
  }
}

static void loadPubChangeConfig() {
  Preferences prefs;
  prefs.begin(NVS_NS, true);
  String raw = prefs.getString(KEY_POC, "[]");
  prefs.end();
  JsonDocument doc;
  if (raw.length() > 2 && deserializeJson(doc, raw) == DeserializationError::Ok) {
    for (JsonObject o : doc.as<JsonArray>()) {
      uint8_t pin = o["pin"].as<uint8_t>();
      const char* topic = o["topic"] | "";
      const char* high  = o["high"] | "1";
      const char* low   = o["low"]  | "0";
      if (strlen(topic) == 0) continue;
      JsonDocument a;
      a["pin"] = pin; a["topic"] = topic; a["high_msg"] = high; a["low_msg"] = low;
      mqttPublishOnChangeHandler(a.as<JsonObject>());
    }
  }
}

bool hasPersistentConfig() {
  Preferences prefs;
  prefs.begin(NVS_NS, true);
  uint8_t v = prefs.getUChar(KEY_SET, 0);
  prefs.end();
  return v == 1;
}

void resetFactoryConfig() {
  Preferences prefs;
  prefs.begin(NVS_NS, false);
  prefs.remove(KEY_MON);
  prefs.remove(KEY_BLK);
  prefs.remove(KEY_GIO);
  prefs.remove(KEY_GPO);
  prefs.remove(KEY_MQT);
  prefs.remove(KEY_SET);
  prefs.remove(KEY_PWM);
  prefs.remove(KEY_DBN);
  prefs.remove(KEY_POC);
  prefs.remove(KEY_FIL);
  prefs.end();
  Serial.println(F("[NVS] Factory reset — next boot will be completely empty"));
}

void clearPersistentConfig() {
  Preferences prefs;
  prefs.begin(NVS_NS, false);
  prefs.putString(KEY_MON, "[]");
  prefs.putString(KEY_BLK, "[]");
  prefs.putString(KEY_GIO, "{}");
  prefs.putString(KEY_GPO, "{}");
  prefs.putString(KEY_MQT, "[]");
  prefs.putString(KEY_PWM, "{}");
  prefs.putString(KEY_DBN, "[]");
  prefs.putString(KEY_POC, "[]");
  prefs.putString(KEY_FIL, "{}");
  prefs.putUChar(KEY_SET, 1);
  prefs.end();
  Serial.println(F("[NVS] Config cleared (factory default disabled until reset_factory)"));
}

String persistJson() {
  JsonDocument doc;
  JsonObject root = doc.to<JsonObject>();

  // GPIO modes from NVS
  {
    Preferences prefs;
    prefs.begin(NVS_NS, true);
    String raw = prefs.getString(KEY_GIO, "{}");
    prefs.end();
    JsonDocument d;
    if (raw.length() > 0) deserializeJson(d, raw);
    root["gpio_modes"] = d.as<JsonObject>();
  }

  // GPIO output values from NVS
  {
    Preferences prefs;
    prefs.begin(NVS_NS, true);
    String raw = prefs.getString(KEY_GPO, "{}");
    prefs.end();
    JsonDocument d;
    if (raw.length() > 0) deserializeJson(d, raw);
    root["gpio_outputs"] = d.as<JsonObject>();
  }

  // Active blinks
  JsonArray blinks = root["blinks"].to<JsonArray>();
  for (uint8_t i = 0; i < MAX_BLINK_TASKS; i++) {
    if (!blinkSlots[i].active) continue;
    JsonObject b = blinks.add<JsonObject>();
    b["pin"]    = blinkSlots[i].pin;
    b["on_ms"]  = blinkSlots[i].onMs;
    b["off_ms"] = blinkSlots[i].offMs;
  }

  // Active monitors
  JsonArray mons = root["monitors"].to<JsonArray>();
  for (uint8_t i = 0; i < MAX_MONITOR_TASKS; i++) {
    if (!monitorSlots[i].active) continue;
    JsonObject m = mons.add<JsonObject>();
    m["pin"]     = monitorSlots[i].pin;
    const char* t = "low";
    if      (monitorSlots[i].trigger == MonitorTrigger::TRIG_HIGH)   t = "high";
    else if (monitorSlots[i].trigger == MonitorTrigger::TRIG_CHANGE) t = "change";
    m["trigger"] = t;
    m["action"]  = monitorSlots[i].actionTool;
    m["once"]    = monitorSlots[i].once;
    if (strlen(monitorSlots[i].actionArgs) > 2) {
      JsonDocument ad;
      if (deserializeJson(ad, monitorSlots[i].actionArgs) == DeserializationError::Ok)
        m["args"] = ad.as<JsonObject>();
    }
  }

  // MQTT subs
  JsonArray subs = root["subs"].to<JsonArray>();
  for (uint8_t i = 0; i < MAX_MQTT_SUBS; i++) {
    if (!mqttSubSlots[i].active) continue;
    JsonObject s = subs.add<JsonObject>();
    s["topic"]  = mqttSubSlots[i].topic;
    s["action"] = mqttSubSlots[i].actionTool;
    if (strlen(mqttSubSlots[i].actionArgs) > 2) {
      JsonDocument ad;
      if (deserializeJson(ad, mqttSubSlots[i].actionArgs) == DeserializationError::Ok)
        s["args"] = ad.as<JsonObject>();
    }
  }

  // Active PWM
  JsonArray pwms = root["pwms"].to<JsonArray>();
  for (uint8_t i = 0; i < MAX_PWM_CHANNELS; i++) {
    if (!pwmSlots[i].active) continue;
    JsonObject p = pwms.add<JsonObject>();
    p["pin"]   = pwmSlots[i].pin;
    p["duty"]  = pwmSlots[i].duty;
    p["freq"]  = pwmSlots[i].freq;
  }

  // Active servos
  JsonArray svos = root["servos"].to<JsonArray>();
  for (uint8_t i = 0; i < MAX_SERVOS; i++) {
    if (!servoSlots[i].active) continue;
    JsonObject s = svos.add<JsonObject>();
    s["pin"] = servoSlots[i].pin;
  }
  // (angle is not stored, servo is set-and-forget)

  // Active debounces
  JsonArray dbns = root["debounces"].to<JsonArray>();
  for (uint8_t i = 0; i < MAX_DEBOUNCE; i++) {
    if (!debounceSlots[i].active) continue;
    JsonObject d = dbns.add<JsonObject>();
    d["pin"]    = debounceSlots[i].pin;
    d["ms"]     = debounceSlots[i].ms;
    d["action"] = debounceSlots[i].actionTool;
    if (strlen(debounceSlots[i].actionArgs) > 2) {
      JsonDocument ad;
      if (deserializeJson(ad, debounceSlots[i].actionArgs) == DeserializationError::Ok)
        d["args"] = ad.as<JsonObject>();
    }
  }

  // Active publish-on-change
  JsonArray pocs = root["publish_on_change"].to<JsonArray>();
  for (uint8_t i = 0; i < MAX_PUBCHANGE; i++) {
    if (!pubChangeSlots[i].active) continue;
    JsonObject p = pocs.add<JsonObject>();
    p["pin"]   = pubChangeSlots[i].pin;
    p["topic"] = pubChangeSlots[i].topic;
    p["high"]  = pubChangeSlots[i].highMsg;
    p["low"]   = pubChangeSlots[i].lowMsg;
  }

  // Files stored in NVS
  {
    Preferences prefs;
    prefs.begin(NVS_NS, true);
    String raw = prefs.getString(KEY_FIL, "{}");
    prefs.end();
    JsonDocument d;
    if (raw.length() > 2) deserializeJson(d, raw);
    root["files"] = d.as<JsonObject>();
  }

  String json;
  serializeJson(root, json);
  return json;
}

}  // namespace skill