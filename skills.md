````md
# ArduClaw Skills Specification
# skills.md

## Overview

ArduClaw menggunakan sistem "Skills" seperti OpenClaw, tetapi fokus pada hardware ESP32 dan IoT automation.

Skill adalah modul kemampuan yang dapat dipanggil oleh AI agent melalui structured tool calling.

Semua akses hardware WAJIB melalui skill layer.
AI TIDAK boleh mengakses GPIO/peripheral secara langsung.

---

# Core Principles

- Hardware abstraction first
- Permission based access
- Safe execution
- JSON tool calling
- Event driven
- Extensible plugin architecture

---

# Skill Execution Flow

```text
User Prompt
    ↓
LLM Planner
    ↓
Tool Call JSON
    ↓
Skill Executor
    ↓
Permission Validator
    ↓
HAL (Hardware Abstraction Layer)
    ↓
ESP32 Peripheral
````

---

# Skill Schema

Semua skill mengikuti format:

```json
{
  "tool": "skill.name",
  "args": {}
}
```

---

# GPIO Skills

## gpio.write

Write digital output to GPIO pin.

### Parameters

```json
{
  "pin": 12,
  "value": true
}
```

### Example

```json
{
  "tool": "gpio.write",
  "args": {
    "pin": 12,
    "value": true
  }
}
```

### Internal Execution

```cpp
hal.digitalWrite(pin, HIGH);
```

---

## gpio.read

Read digital input from GPIO.

### Parameters

```json
{
  "pin": 14
}
```

### Response

```json
{
  "value": true
}
```

---

## gpio.mode

Configure GPIO mode.

### Parameters

```json
{
  "pin": 12,
  "mode": "output"
}
```

### Supported Modes

* input
* output
* input_pullup
* input_pulldown

---

# Analog Skills

## adc.read

Read analog value from ADC pin.

### Parameters

```json
{
  "pin": 34
}
```

### Response

```json
{
  "raw": 2840,
  "voltage": 2.29
}
```

---

# PWM Skills

## pwm.attach

Attach PWM channel to pin.

### Parameters

```json
{
  "pin": 18,
  "channel": 0,
  "frequency": 5000,
  "resolution": 8
}
```

---

## pwm.write

Write PWM duty cycle.

### Parameters

```json
{
  "pin": 18,
  "duty": 128
}
```

---

# I2C Skills

## i2c.scan

Scan all I2C devices.

### Response

```json
{
  "devices": [
    "0x3C",
    "0x68"
  ]
}
```

---

## i2c.read

Read register from I2C device.

### Parameters

```json
{
  "address": "0x68",
  "register": "0x75"
}
```

---

## i2c.write

Write register to I2C device.

### Parameters

```json
{
  "address": "0x68",
  "register": "0x6B",
  "value": 0
}
```

---

# SPI Skills

## spi.transfer

Transfer SPI data.

### Parameters

```json
{
  "data": [1,2,3]
}
```

---

# UART Skills

## serial.write

Write serial data.

### Parameters

```json
{
  "port": 1,
  "data": "hello"
}
```

---

## serial.read

Read serial data.

### Parameters

```json
{
  "port": 1
}
```

---

# WiFi Skills

## wifi.connect

Connect to WiFi.

### Parameters

```json
{
  "ssid": "MyWiFi",
  "password": "secret"
}
```

---

## wifi.status

Get WiFi connection status.

### Response

```json
{
  "connected": true,
  "ip": "192.168.1.20",
  "rssi": -48
}
```

---

# HTTP Skills

## http.get

Perform HTTP GET request.

### Parameters

```json
{
  "url": "https://example.com"
}
```

---

## http.post

Perform HTTP POST request.

### Parameters

```json
{
  "url": "https://api.example.com",
  "body": {}
}
```

---

# MQTT Skills

## mqtt.publish

Publish MQTT message.

### Parameters

```json
{
  "topic": "home/lamp",
  "payload": "ON"
}
```

---

## mqtt.subscribe

Subscribe to MQTT topic.

### Parameters

```json
{
  "topic": "home/sensor/temp"
}
```

---

# Device Skills

Device skills abstract low-level hardware.

---

## device.relay

Control relay module.

### Parameters

```json
{
  "pin": 26,
  "state": true
}
```

---

## device.servo

Move servo motor.

### Parameters

```json
{
  "pin": 18,
  "angle": 90
}
```

---

## device.oled.print

Display text on OLED.

### Parameters

```json
{
  "text": "Hello ArduClaw"
}
```

---

## device.dht22.read

Read DHT22 sensor.

### Parameters

```json
{
  "pin": 4
}
```

### Response

```json
{
  "temperature": 29.5,
  "humidity": 71
}
```

---

# Event Engine

ArduClaw supports rule based automation.

---

## Rule Schema

```json
{
  "if": {
    "sensor": "temperature",
    "operator": ">",
    "value": 30
  },
  "then": {
    "tool": "gpio.write",
    "args": {
      "pin": 12,
      "value": true
    }
  }
}
```

---

# Planner Layer

Planner converts natural language into execution plan.

---

## Example Prompt

```text
Turn on fan if temperature exceeds 30C
```

---

## Generated Plan

```json
[
  {
    "tool": "device.dht22.read",
    "args": {
      "pin": 4
    }
  },
  {
    "tool": "gpio.write",
    "args": {
      "pin": 12,
      "value": true
    },
    "condition": "temperature > 30"
  }
]
```

---

# Skill Registry

All skills must be registered before use.

---

## Example

```cpp
registerTool("gpio.write", gpioWriteHandler);
registerTool("adc.read", adcReadHandler);
registerTool("device.servo", servoHandler);
```

---

# Permissions

All skills are permission based.

---

## Example Policy

```json
{
  "allowedPins": [2,4,5,12,18,26],
  "forbiddenPins": [0,1,3,6,7,8,9,10,11],
  "maxPwmFrequency": 10000
}
```

---

# Safety Features

## Required Safety

* pin whitelist
* rate limiter
* watchdog timer
* safe mode
* sandboxed execution
* execution timeout
* memory limit

---

# Recommended Architecture

```text
ESP32
 ├── HAL
 ├── Skill Runtime
 ├── MQTT Client
 ├── Event Engine
 ├── Rule Engine
 └── Device Drivers
```

---

# Communication Protocol

Recommended protocols:

* MQTT
* WebSocket
* REST API
* Serial JSON

---

# Recommended Stack

## Firmware

* ESP-IDF
* Arduino Framework
* FreeRTOS

---

## AI Runtime

* Ollama
* llama.cpp
* OpenAI API

---

# Future Features

* OTA updates
* multi-agent coordination
* Home Assistant integration
* camera vision
* voice control
* memory persistence
* autonomous planning
* remote orchestration
* plugin marketplace

---

# Example Full Workflow

## User Prompt

```text
Read temperature every 5 seconds and turn on fan above 30C
```

---

## Planner Output

```json
[
  {
    "tool": "device.dht22.read",
    "args": {
      "pin": 4
    }
  },
  {
    "tool": "gpio.write",
    "args": {
      "pin": 12,
      "value": true
    },
    "condition": "temperature > 30"
  }
]
```

---

## Executor Runtime

```cpp
float temp = dht.readTemperature();

if(temp > 30){
    hal.digitalWrite(12, HIGH);
}
```

---

# End of skills.md

```
```
