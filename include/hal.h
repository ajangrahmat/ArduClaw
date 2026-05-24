#ifndef ARDUCLAW_HAL_H
#define ARDUCLAW_HAL_H

#include <Arduino.h>
#include <cstdint>

/**
 * ArduClaw Hardware Abstraction Layer (HAL)
 * 
 * Provides safe, centralized access to ESP32 hardware peripherals.
 * All hardware access MUST go through this layer.
 */

namespace hal {

// ─────────────────────────────────────────────────────────────────────────
// GPIO - Digital Input/Output
// ─────────────────────────────────────────────────────────────────────────

enum class PinMode : uint8_t {
    INPUT,
    OUTPUT,
    INPUT_PULLUP,
    INPUT_PULLDOWN
};

/**
 * Configure GPIO pin mode
 * @param pin GPIO pin number
 * @param mode INPUT, OUTPUT, INPUT_PULLUP, or INPUT_PULLDOWN
 * @return true if successful
 */
bool gpioSetMode(uint8_t pin, PinMode mode);

/**
 * Write digital value to GPIO pin
 * @param pin GPIO pin number
 * @param value true (HIGH) or false (LOW)
 * @return true if successful
 */
bool gpioWrite(uint8_t pin, bool value);

/**
 * Read digital value from GPIO pin
 * @param pin GPIO pin number
 * @param outValue pointer to store the read value
 * @return true if successful
 */
bool gpioRead(uint8_t pin, bool& outValue);

/**
 * Get GPIO pin state (without reading)
 * @param pin GPIO pin number
 * @return current pin state (true/false)
 */
bool gpioGetState(uint8_t pin);

// ─────────────────────────────────────────────────────────────────────────
// ADC - Analog Input
// ─────────────────────────────────────────────────────────────────────────

struct AdcReading {
    int16_t  raw;      // Raw ADC value (0-4095 for 12-bit)
    float    voltage;  // Voltage reading (0.0 - 3.3V)
};

/**
 * Read analog value from ADC pin
 * @param pin GPIO pin with ADC capability (34, 35, 36, 37, 38, 39)
 * @param outReading pointer to store the reading
 * @return true if successful
 */
bool adcRead(uint8_t pin, AdcReading& outReading);

/**
 * Get raw ADC value
 * @param pin ADC pin number
 * @return raw 12-bit ADC value (0-4095), or -1 on error
 */
int16_t adcReadRaw(uint8_t pin);

/**
 * Get voltage from ADC pin
 * @param pin ADC pin number
 * @return voltage (0.0 - 3.3V), or -1.0 on error
 */
float adcReadVoltage(uint8_t pin);

// ─────────────────────────────────────────────────────────────────────────
// HAL Initialization
// ─────────────────────────────────────────────────────────────────────────

/**
 * Initialize HAL system
 * Call this once in setup()
 */
void init();

/**
 * Get HAL status
 * @return true if HAL initialized successfully
 */
bool isInitialized();

}  // namespace hal

#endif  // ARDUCLAW_HAL_H
