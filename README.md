
# ArduClaw

> Lightweight AI Automation Runtime for ESP32 & Arduino

ArduClaw adalah framework automation ringan untuk ESP32 dan Arduino yang terinspirasi dari konsep modern edge automation seperti ESP-Claw, tetapi dibuat lebih sederhana, lebih ringan, dan lebih mudah digunakan pada microcontroller dengan RAM kecil.

ArduClaw fokus pada:
- Event-driven automation
- Rule-based actions
- Lightweight runtime
- Web-based setup
- AI/LLM integration (optional)
- Beginner-friendly workflow

---

# ✨ Features

## 🚀 Lightweight Runtime
- Tidak membutuhkan PSRAM
- Cocok untuk ESP32 biasa
- RAM usage rendah
- Fast boot

---

## ⚡ Event System
Automation berbasis event.

Contoh:
```cpp
emit("motion_detected");
````

---

## 🧠 Rule Engine

Membuat automation sederhana.

```cpp
when("motion_detected", turnOnLamp);
```

---

## 🌐 Web Flasher

Upload firmware `.bin` langsung dari browser.

Fitur:

* drag & drop firmware
* Web Serial API
* flash progress
* auto reboot

---

## 📶 WiFi Setup

Setup WiFi tanpa recompile firmware.

---

## 🤖 AI / LLM Integration

Optional AI integration:

* OpenAI
* Gemini
* OpenRouter
* Ollama
* Custom API

---

## 🖥️ Web Dashboard

Simple browser dashboard untuk:

* monitoring
* GPIO control
* serial terminal
* event logs
* OTA update

---

## 🔌 Modular Architecture

Module dapat diaktif/nonaktifkan sesuai kebutuhan.

---

# 🎯 Goals

ArduClaw dibuat untuk:

* maker
* hobbyist
* pelajar
* project IoT
* smart home
* wearable
* robot sederhana

Dengan fokus:

```text
Simple
Lightweight
Modular
Easy to Use
```

---

# 🧠 Philosophy

> "Small Devices, Smart Automation"

ArduClaw tidak mencoba menjadi AI runtime besar.

Tujuannya adalah membawa konsep automation modern ke:

* ESP32 murah
* board kecil
* project sederhana
* Arduino ecosystem

---

# ⚙️ Development Environment

## IDE

* Visual Studio Code

## Extension

* PlatformIO IDE

## Framework

* Arduino Framework

---

# 📦 Project Structure

```text
ArduClaw/
├── include/
├── lib/
│   ├── ArduClawCore/
│   ├── ArduClawWiFi/
│   ├── ArduClawMQTT/
│   ├── ArduClawLLM/
│   └── ArduClawDashboard/
│
├── src/
├── data/
├── docs/
├── webflasher/
├── test/
└── platformio.ini
```

---

# 🔥 Core Architecture

```text
Sensor/Input
      ↓
Event System
      ↓
Rule Engine
      ↓
Action Executor
      ↓
GPIO / MQTT / API / LLM
```

---

# 🔌 Example Usage

## Event

```cpp
emit("door_open");
```

---

## Rule

```cpp
when("door_open", turnOnLamp);
```

---

## Action

```cpp
void turnOnLamp() {
  digitalWrite(RELAY_PIN, HIGH);
}
```

---

# 🌐 Web Interface

ArduClaw menyediakan web interface sederhana untuk:

## Firmware Flasher

* upload `.bin`
* flash ESP32
* erase flash

## WiFi Setup

* SSID
* password
* reconnect settings

## LLM Setup

* provider
* API key
* endpoint
* model

## Dashboard

* live logs
* device status
* RAM usage
* GPIO control

## Serial Terminal

* realtime serial monitor
* command input

---

# 🔧 Serial Commands

## WiFi

```text
wifi set MyWiFi password123
wifi status
wifi reset
```

---

## LLM

```text
llm provider openai
llm api_key sk-xxxx
llm model gpt-4o-mini
```

---

## Device

```text
device info
device restart
```

---

## Event

```text
emit relay_on
```

---

# 📡 Supported Boards

| Board        | Status       |
| ------------ | ------------ |
| ESP32 DevKit | ✅            |
| ESP32-WROOM  | ✅            |
| ESP32-C3     | ✅            |
| ESP32-S2     | ⚠️           |
| ESP8266      | Experimental |

---

# 📉 Resource Target

## RAM Usage

```text
~50KB - 120KB
```

## Flash Usage

```text
~500KB - 1.5MB
```

---

# 🎨 UI Concept

ArduClaw menggunakan UI:

* modern minimal
* dark mode
* mobile friendly
* lightweight

Tanpa framework frontend berat.

---

# 🔐 Security Plans

Planned:

* encrypted config
* HTTPS support
* secure OTA
* token authentication

---

# 🛣️ Roadmap

## v0.1

* Event system
* Rule engine
* GPIO actions
* Serial CLI

## v0.2

* WiFi manager
* MQTT
* Telegram integration

## v0.3

* Web flasher
* OTA update
* Dashboard

## v0.4

* AI/LLM integration
* Visual automation
* AI assistant

## v1.0

* Stable release
* Full documentation
* Plugin ecosystem

---

# 🔥 Positioning

ArduClaw is:

```text
ESP-Claw inspired
lightweight automation runtime
for smaller ESP devices.
```

---

# ❤️ Why ArduClaw?

Because not every ESP32 project needs:

* local AI runtime
* PSRAM besar
* complex agent system

Sometimes you just need:

```text
Simple Automation
Fast Deployment
Modern Setup
Lightweight Runtime
```

---

# 📜 License

MIT License

---

# ❤️ ArduClaw

Small Devices, Smart Automation.