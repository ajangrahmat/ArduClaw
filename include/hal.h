#ifndef ARDUCLAW_HAL_H
#define ARDUCLAW_HAL_H

#include <Arduino.h>
#include <cstdint>

/**
 * ArduClaw Hardware Abstraction Layer (HAL)
 *
 * Provides safe, centralized access to ESP32 hardware peripherals.
 * All hardware access MUST go through this layer.
 *
 * ╔══════════════════════════════════════════════════════════════╗
 * ║  STRAPPING PINS — JANGAN DIPAKAI UNTUK GPIO OUTPUT:         ║
 * ║  GPIO 0, 2, 5, 12, 15                                       ║
 * ║                                                              ║
 * ║  GPIO AMAN UNTUK OUTPUT:                                     ║
 * ║  13, 14, 16, 17, 18, 19, 21, 25, 26, 27, 32, 33            ║
 * ║                                                              ║
 * ║  ADC AMAN (tidak bentrok WiFi):                              ║
 * ║  32, 33, 34, 35, 36, 39  (ADC1)                             ║
 * ║  ADC2 (25,26,27,32,33) TIDAK BISA saat WiFi aktif           ║
 * ╚══════════════════════════════════════════════════════════════╝
 */

namespace hal {

// ─────────────────────────────────────────────────────────────────────────
// GPIO
// ─────────────────────────────────────────────────────────────────────────

enum class PinMode : uint8_t {
  PIN_INPUT,
  PIN_OUTPUT,
  PIN_INPUT_PULLUP,
  PIN_INPUT_PULLDOWN
};

bool gpioSetMode(uint8_t pin, PinMode mode);
bool gpioWrite(uint8_t pin, bool value);
bool gpioRead(uint8_t pin, bool& outValue);
bool gpioGetState(uint8_t pin);

// ─────────────────────────────────────────────────────────────────────────
// ADC
// ─────────────────────────────────────────────────────────────────────────

struct AdcReading {
  int16_t raw;      // 0–4095
  float   voltage;  // 0.0–3.3V
};

bool    adcRead(uint8_t pin, AdcReading& outReading);
int16_t adcReadRaw(uint8_t pin);
float   adcReadVoltage(uint8_t pin);

// ─────────────────────────────────────────────────────────────────────────
// Init
// ─────────────────────────────────────────────────────────────────────────

void init();
bool isInitialized();

}  // namespace hal

#endif  // ARDUCLAW_HAL_H