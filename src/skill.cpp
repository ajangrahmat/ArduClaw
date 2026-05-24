#include "skill.h"
#include "hal.h"
#include <map>

namespace skill {

// ─────────────────────────────────────────────────────────────────────────
// Skill Registry (Map of skill name -> handler)
// ─────────────────────────────────────────────────────────────────────────

static std::map<String, SkillHandler> skillRegistry;
static bool _initialized = false;

// ─────────────────────────────────────────────────────────────────────────
// Built-in Skill Handlers - GPIO
// ─────────────────────────────────────────────────────────────────────────

SkillResult gpioWriteHandler(const JsonObject& args) {
    SkillResult result{false, "Missing pin or value"};
    
    if (!args.containsKey("pin") || !args.containsKey("value")) {
        return result;
    }
    
    uint8_t pin = args["pin"];
    bool value = args["value"];
    
    if (hal::gpioWrite(pin, value)) {
        result.success = true;
        result.message = "GPIO " + String(pin) + " set to " + String(value ? "HIGH" : "LOW");
    } else {
        result.message = "Failed to write GPIO " + String(pin);
    }
    
    return result;
}

SkillResult gpioReadHandler(const JsonObject& args) {
    SkillResult result{false, "Missing pin"};
    
    if (!args.containsKey("pin")) {
        return result;
    }
    
    uint8_t pin = args["pin"];
    bool value = hal::gpioGetState(pin);
    
    result.success = true;
    result.message = "GPIO " + String(pin) + " = " + String(value ? "HIGH" : "LOW");
    result.responseData["value"] = value;
    
    return result;
}

SkillResult gpioModeHandler(const JsonObject& args) {
    SkillResult result{false, "Missing pin or mode"};
    
    if (!args.containsKey("pin") || !args.containsKey("mode")) {
        return result;
    }
    
    uint8_t pin = args["pin"];
    const char* modeStr = args["mode"];
    
    hal::PinMode mode = hal::PinMode::INPUT;
    if (strcmp(modeStr, "output") == 0) {
        mode = hal::PinMode::OUTPUT;
    } else if (strcmp(modeStr, "input_pullup") == 0) {
        mode = hal::PinMode::INPUT_PULLUP;
    } else if (strcmp(modeStr, "input_pulldown") == 0) {
        mode = hal::PinMode::INPUT_PULLDOWN;
    }
    
    if (hal::gpioSetMode(pin, mode)) {
        result.success = true;
        result.message = "GPIO " + String(pin) + " mode set to " + String(modeStr);
    } else {
        result.message = "Failed to set GPIO " + String(pin) + " mode";
    }
    
    return result;
}

// ─────────────────────────────────────────────────────────────────────────
// Built-in Skill Handlers - ADC
// ─────────────────────────────────────────────────────────────────────────

SkillResult adcReadHandler(const JsonObject& args) {
    SkillResult result{false, "Missing pin"};
    
    if (!args.containsKey("pin")) {
        return result;
    }
    
    uint8_t pin = args["pin"];
    hal::AdcReading reading{};
    
    if (hal::adcRead(pin, reading)) {
        result.success = true;
        result.message = "ADC pin " + String(pin) + " read successfully";
        result.responseData["raw"] = reading.raw;
        result.responseData["voltage"] = reading.voltage;
    } else {
        result.message = "Failed to read ADC pin " + String(pin);
    }
    
    return result;
}

// ─────────────────────────────────────────────────────────────────────────
// System Skills
// ─────────────────────────────────────────────────────────────────────────

SkillResult systemStatusHandler(const JsonObject& args) {
    SkillResult result{true, "System status retrieved"};
    result.responseData["uptime"] = millis() / 1000;
    result.responseData["heap"] = ESP.getFreeHeap();
    return result;
}

// ─────────────────────────────────────────────────────────────────────────
// Skill Registry Implementation
// ─────────────────────────────────────────────────────────────────────────

void registerSkill(const String& name, SkillHandler handler) {
    skillRegistry[name] = handler;
}

bool hasSkill(const String& name) {
    return skillRegistry.find(name) != skillRegistry.end();
}

String listSkills() {
    String list = "";
    for (auto& pair : skillRegistry) {
        if (list.length() > 0) list += ", ";
        list += pair.first;
    }
    return list;
}

// ─────────────────────────────────────────────────────────────────────────
// Skill Execution
// ─────────────────────────────────────────────────────────────────────────

SkillResult executeSkill(const JsonObject& skillJson) {
    SkillResult result{false, "Invalid skill format"};
    
    if (!skillJson.containsKey("tool")) {
        result.message = "Missing 'tool' field";
        return result;
    }
    
    const char* toolName = skillJson["tool"];
    JsonObject args = skillJson.containsKey("args") 
                        ? skillJson["args"].as<JsonObject>()
                        : JsonObject();
    
    return executeSkillByName(toolName, args);
}

SkillResult executeSkillByName(const String& name, const JsonObject& args) {
    SkillResult result{false, "Skill not found: " + name};
    
    auto it = skillRegistry.find(name);
    if (it == skillRegistry.end()) {
        return result;
    }
    
    // Execute handler with error handling
    try {
        return it->second(args);
    } catch (...) {
        result.message = "Skill execution error: " + name;
        return result;
    }
}

bool executeSkillChain(const JsonArray& skillArray, JsonArray& results) {
    bool allSuccess = true;
    
    for (JsonObject skillObj : skillArray) {
        SkillResult result = executeSkill(skillObj);
        
        JsonObject resultObj = results.createNestedObject();
        resultObj["success"] = result.success;
        resultObj["message"] = result.message;
        
        if (!result.success) {
            allSuccess = false;
        }
    }
    
    return allSuccess;
}

// ─────────────────────────────────────────────────────────────────────────
// Skill System Initialization
// ─────────────────────────────────────────────────────────────────────────

void init() {
    if (_initialized) return;
    
    // Register GPIO skills
    registerSkill("gpio.write", gpioWriteHandler);
    registerSkill("gpio.read", gpioReadHandler);
    registerSkill("gpio.mode", gpioModeHandler);
    
    // Register ADC skills
    registerSkill("adc.read", adcReadHandler);
    
    // Register system skills
    registerSkill("system.status", systemStatusHandler);
    
    _initialized = true;
}

bool isInitialized() {
    return _initialized;
}

}  // namespace skill
