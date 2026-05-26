#include "skill.h"
#include "hal.h"
#include <Preferences.h>

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

static const uint8_t MAX_SKILLS = 20;

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
    val = (strcmp(s, "on") == 0 || strcmp(s, "ON") == 0 ||
           strcmp(s, "high") == 0 || strcmp(s, "HIGH") == 0 ||
           strcmp(s, "1") == 0 || strcmp(s, "true") == 0);
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
  char msg[80];
  snprintf(msg, sizeof(msg), "GPIO %d blink dimulai (on=%ums off=%ums)", pin, on_ms, off_ms);
  Serial.printf("[SKILL] gpio.blink_start: %s\n", msg);
  return {true, String(msg) + warn};
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
    char msg[48];
    snprintf(msg, sizeof(msg), "Semua blink task dihentikan (%u task)", stopped);
    return {stopped > 0, String(msg)};
  }
  uint8_t    pin = args["pin"].as<uint8_t>();
  BlinkSlot* s   = findBlinkSlotByPin(pin);
  if (!s) return {false, "Tidak ada blink task aktif di pin " + String(pin)};
  s->stopRequest = true;
  for (int i = 0; i < 30 && s->active; i++) vTaskDelay(pdMS_TO_TICKS(50));
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
// MQTT API — dipanggil dari main.cpp (mqtt_task)
// ─────────────────────────────────────────────────────────────────────────

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

static const char* NVS_NS  = "ac";
static const char* KEY_MON = "mons";    // JSON array of monitor configs
static const char* KEY_GIO = "gpiocfg"; // JSON object pin→mode
static const char* KEY_MQT = "mqts";    // JSON array of MQTT subscriptions
static const char* KEY_SET = "cfg_set"; // uint8_t: 1 jika pernah dikonfigurasi

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
  regCount = 0;

  registerSkill("gpio.write",        gpioWriteHandler);
  registerSkill("gpio.read",         gpioReadHandler);
  registerSkill("gpio.mode",         gpioModeHandler);
  registerSkill("gpio.blink",        gpioBlinkHandler);
  registerSkill("gpio.blink_start",  gpioBlinkStartHandler);
  registerSkill("gpio.blink_stop",   gpioBlinkStopHandler);
  registerSkill("gpio.monitor",      gpioMonitorHandler);
  registerSkill("gpio.monitor_stop", gpioMonitorStopHandler);
  registerSkill("adc.read",          adcReadHandler);
  registerSkill("system.status",        systemStatusHandler);
  registerSkill("system.clear_persist",  systemClearPersistHandler);
  registerSkill("system.reset_factory",  systemResetFactoryHandler);
  registerSkill("mqtt.publish",       mqttPublishHandler);
  registerSkill("mqtt.subscribe",     mqttSubscribeHandler);
  registerSkill("mqtt.unsubscribe",   mqttUnsubscribeHandler);
  registerSkill("mqtt.status",        mqttStatusHandler);
  registerSkill("mqtt.reconnect",     mqttReconnectHandler);
  registerSkill("mqtt.listsubs",      mqttListSubsHandler);

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

  Serial.printf("[NVS] Restored %u items (monitors+gpio+mqtt)\n", restored);
  return restored;
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
  prefs.remove(KEY_GIO);
  prefs.remove(KEY_MQT);
  prefs.remove(KEY_SET);
  prefs.end();
  Serial.println(F("[NVS] Factory reset — next boot will be completely empty"));
}

void clearPersistentConfig() {
  Preferences prefs;
  prefs.begin(NVS_NS, false);
  prefs.putString(KEY_MON, "[]");
  prefs.putString(KEY_GIO, "{}");
  prefs.putString(KEY_MQT, "[]");
  prefs.putUChar(KEY_SET, 1);
  prefs.end();
  Serial.println(F("[NVS] Config cleared (factory default disabled until reset_factory)"));
}

}  // namespace skill