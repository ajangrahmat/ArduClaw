#ifndef ARDUCLAW_SKILL_H
#define ARDUCLAW_SKILL_H

#include <Arduino.h>
#include <functional>
#include <ArduinoJson.h>

/**
 * ArduClaw Skill System
 * 
 * - Centralized skill registry
 * - JSON tool calling from LLM
 * - Safe skill execution with validation
 * - Permission-based access control
 */

namespace skill {

// ─────────────────────────────────────────────────────────────────────────
// Skill Execution Result
// ─────────────────────────────────────────────────────────────────────────

struct SkillResult {
    bool success;
    String message;
    JsonDocument responseData;  // Optional: skill-specific response
};

// ─────────────────────────────────────────────────────────────────────────
// Skill Handler Type
// ─────────────────────────────────────────────────────────────────────────

/**
 * Skill handler function signature
 * Receives JSON args, executes skill, returns result
 * 
 * Example:
 *   SkillResult gpioWriteHandler(const JsonObject& args) {
 *       int pin = args["pin"];
 *       bool value = args["value"];
 *       hal::gpioWrite(pin, value);
 *       return {true, "GPIO written"};
 *   }
 */
using SkillHandler = std::function<SkillResult(const JsonObject&)>;

// ─────────────────────────────────────────────────────────────────────────
// Skill Registry
// ─────────────────────────────────────────────────────────────────────────

/**
 * Register a skill handler
 * @param name Skill name (e.g., "gpio.write", "adc.read")
 * @param handler Function to execute skill
 */
void registerSkill(const String& name, SkillHandler handler);

/**
 * Check if skill is registered
 * @param name Skill name
 * @return true if registered
 */
bool hasSkill(const String& name);

/**
 * Get list of registered skills
 * @return comma-separated skill names
 */
String listSkills();

// ─────────────────────────────────────────────────────────────────────────
// Skill Execution
// ─────────────────────────────────────────────────────────────────────────

/**
 * Execute a skill by name with JSON arguments
 * 
 * JSON Format:
 * {
 *   "tool": "skill.name",
 *   "args": { ... }
 * }
 * 
 * @param skillJson JSON object with "tool" and "args"
 * @return SkillResult with success status and response
 */
SkillResult executeSkill(const JsonObject& skillJson);

/**
 * Execute skill by name and args separately
 * @param name Skill name
 * @param args JSON arguments
 * @return SkillResult
 */
SkillResult executeSkillByName(const String& name, const JsonObject& args);

/**
 * Parse and execute multiple skills from JSON array
 * Useful for LLM tool calling chains
 * 
 * Example:
 * [
 *   {"tool": "gpio.write", "args": {"pin": 12, "value": true}},
 *   {"tool": "adc.read", "args": {"pin": 34}}
 * ]
 * 
 * @param skillArray JSON array of skills
 * @param results Output array of results
 * @return true if all executed successfully
 */
bool executeSkillChain(const JsonArray& skillArray, JsonArray& results);

// ─────────────────────────────────────────────────────────────────────────
// Skill System Initialization
// ─────────────────────────────────────────────────────────────────────────

/**
 * Initialize skill system and register all built-in skills
 */
void init();

/**
 * Get skill system status
 */
bool isInitialized();

}  // namespace skill

#endif  // ARDUCLAW_SKILL_H
