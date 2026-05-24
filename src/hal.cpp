#include "hal.h"

namespace hal {

static bool _initialized = false;

// ─────────────────────────────────────────────────────────────────────────
// HAL Initialization
// ─────────────────────────────────────────────────────────────────────────

void init() {
    // Basic ESP32 pins are already initialized by Arduino framework
    // Add any additional HAL setup here
    _initialized = true;
}

bool isInitialized() {
    return _initialized;
}

// ─────────────────────────────────────────────────────────────────────────
// GPIO - Digital I/O Implementation
// ─────────────────────────────────────────────────────────────────────────

bool gpioSetMode(uint8_t pin, PinMode mode) {
    if (!_initialized) return false;
    
    uint8_t pinMode_value = INPUT;
    switch (mode) {
        case PinMode::INPUT:
            pinMode_value = INPUT;
            break;
        case PinMode::OUTPUT:
            pinMode_value = OUTPUT;
            break;
        case PinMode::INPUT_PULLUP:
            pinMode_value = INPUT_PULLUP;
            break;
        case PinMode::INPUT_PULLDOWN:
            pinMode_value = INPUT_PULLDOWN;
            break;
    }
    
    pinMode(pin, pinMode_value);
    return true;
}

bool gpioWrite(uint8_t pin, bool value) {
    if (!_initialized) return false;
    
    digitalWrite(pin, value ? HIGH : LOW);
    return true;
}

bool gpioRead(uint8_t pin, bool& outValue) {
    if (!_initialized) return false;
    
    outValue = digitalRead(pin) == HIGH;
    return true;
}

bool gpioGetState(uint8_t pin) {
    if (!_initialized) return false;
    
    return digitalRead(pin) == HIGH;
}

// ─────────────────────────────────────────────────────────────────────────
// ADC - Analog Input Implementation
// ─────────────────────────────────────────────────────────────────────────

// ADC pins on ESP32: 34, 35, 36, 37, 38, 39 (ADC1)
// and 32, 33, 25, 26, 27 (ADC2, but shared with WiFi)
// Voltage range: 0 - 3.3V, but ESP32 ADC range is limited to ~1.1V to 3.0V
// We'll use 3.3V as reference for simplicity

static const float ADC_VREF = 3.3f;  // Reference voltage
static const int16_t ADC_MAX = 4095; // 12-bit ADC

bool adcRead(uint8_t pin, AdcReading& outReading) {
    if (!_initialized) return false;
    
    int16_t raw = analogRead(pin);
    if (raw < 0) return false;
    
    outReading.raw = raw;
    outReading.voltage = (static_cast<float>(raw) / ADC_MAX) * ADC_VREF;
    
    return true;
}

int16_t adcReadRaw(uint8_t pin) {
    if (!_initialized) return -1;
    
    int16_t raw = analogRead(pin);
    return raw;
}

float adcReadVoltage(uint8_t pin) {
    if (!_initialized) return -1.0f;
    
    int16_t raw = analogRead(pin);
    if (raw < 0) return -1.0f;
    
    return (static_cast<float>(raw) / ADC_MAX) * ADC_VREF;
}

}  // namespace hal
