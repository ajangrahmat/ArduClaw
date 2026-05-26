#include "hal.h"
#include <driver/gpio.h>

namespace hal {

static bool _initialized = false;

// ─────────────────────────────────────────────────────────────────────────
// Init
// ─────────────────────────────────────────────────────────────────────────

void init() {
  _initialized = true;
}

bool isInitialized() {
  return _initialized;
}

// ─────────────────────────────────────────────────────────────────────────
// GPIO
// ─────────────────────────────────────────────────────────────────────────

bool gpioSetMode(uint8_t pin, PinMode mode) {
  if (!_initialized) return false;

  gpio_mode_t      gpio_mode = GPIO_MODE_INPUT;
  gpio_pull_mode_t pull_mode = GPIO_FLOATING;

  switch (mode) {
    case PinMode::PIN_INPUT:
      gpio_mode = GPIO_MODE_INPUT;
      pull_mode = GPIO_FLOATING;
      break;
    case PinMode::PIN_OUTPUT:
      gpio_mode = GPIO_MODE_OUTPUT;
      pull_mode = GPIO_FLOATING;
      break;
    case PinMode::PIN_INPUT_PULLUP:
      gpio_mode = GPIO_MODE_INPUT;
      pull_mode = GPIO_PULLUP_ONLY;
      break;
    case PinMode::PIN_INPUT_PULLDOWN:
      gpio_mode = GPIO_MODE_INPUT;
      pull_mode = GPIO_PULLDOWN_ONLY;
      break;
  }

  gpio_config_t io_conf     = {};
  io_conf.intr_type         = GPIO_INTR_DISABLE;
  io_conf.mode              = gpio_mode;
  io_conf.pin_bit_mask      = (1ULL << pin);
  io_conf.pull_down_en      = GPIO_PULLDOWN_DISABLE;
  io_conf.pull_up_en        = GPIO_PULLUP_DISABLE;

  esp_err_t err = gpio_config(&io_conf);
  if (err != ESP_OK) return false;

  gpio_set_pull_mode((gpio_num_t)pin, pull_mode);
  return true;
}

bool gpioWrite(uint8_t pin, bool value) {
  if (!_initialized) return false;

  gpio_config_t io_conf = {};
  io_conf.intr_type     = GPIO_INTR_DISABLE;
  io_conf.mode          = GPIO_MODE_OUTPUT;
  io_conf.pin_bit_mask  = (1ULL << pin);
  io_conf.pull_down_en  = GPIO_PULLDOWN_DISABLE;
  io_conf.pull_up_en    = GPIO_PULLUP_DISABLE;
  gpio_config(&io_conf);

  gpio_set_level((gpio_num_t)pin, value ? 1 : 0);
  return true;
}

bool gpioRead(uint8_t pin, bool& outValue) {
  if (!_initialized) return false;
  outValue = gpio_get_level((gpio_num_t)pin) != 0;
  return true;
}

bool gpioGetState(uint8_t pin) {
  if (!_initialized) return false;
  return gpio_get_level((gpio_num_t)pin) != 0;
}

// ─────────────────────────────────────────────────────────────────────────
// ADC
// ─────────────────────────────────────────────────────────────────────────

static const float   ADC_VREF = 3.3f;
static const int16_t ADC_MAX  = 4095;

bool adcRead(uint8_t pin, AdcReading& outReading) {
  if (!_initialized) return false;
  int raw = analogRead(pin);
  if (raw < 0) return false;
  outReading.raw     = (int16_t)raw;
  outReading.voltage = ((float)raw / ADC_MAX) * ADC_VREF;
  return true;
}

int16_t adcReadRaw(uint8_t pin) {
  if (!_initialized) return -1;
  return (int16_t)analogRead(pin);
}

float adcReadVoltage(uint8_t pin) {
  if (!_initialized) return -1.0f;
  int raw = analogRead(pin);
  if (raw < 0) return -1.0f;
  return ((float)raw / ADC_MAX) * ADC_VREF;
}

}  // namespace hal