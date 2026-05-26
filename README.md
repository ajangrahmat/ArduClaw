
# ArduClaw

> **Lightweight AI Automation Runtime for ESP32 & Arduino**  
> **Runtime Otomatisasi AI Ringan untuk ESP32 & Arduino**

---

## 🌐 EN / ID

<details open>
<summary><b>English</b></summary>

ArduClaw is a lightweight automation runtime for ESP32, inspired by modern edge automation concepts (ESP-Claw). It features an event-driven skill system, LLM/AI integration, MQTT support, web dashboard, and a browser-based firmware flasher — all running on commodity ESP32 hardware without PSRAM.

### Features

- **Lightweight Runtime** — ~50–120KB RAM, ~500KB–1.5MB flash. No PSRAM needed.
- **Event-Driven Skill System** — GPIO, ADC, PWM, I2C, SPI, UART control via JSON tool calling.
- **LLM/AI Integration** — OpenAI, Gemini, OpenRouter, Ollama, or any OpenAI-compatible API.
- **WiFi Manager** — Configure SSID/password without recompiling firmware.
- **MQTT Support** — Publish, subscribe with wildcard matching and action dispatch.
- **Web Dashboard** — Chat with AI, device status, WiFi/LLM/MQTT config, skill list.
- **Blink & Monitor Tasks** — Non-blocking GPIO blink, pin state monitoring with debounce.
- **NVS Persistence** — GPIO modes, monitor rules, and MQTT subscriptions survive reboot.
- **Browser Flasher** — Upload firmware `.bin` via Web Serial API.
- **Multi-Task Architecture** — Dual-core FreeRTOS: Core 0 (loop, skill, mqtt), Core 1 (wifi, llm).

### Project Structure

```
ArduClaw/
├── include/          # Headers: hal.h, skill.h, dashboard.h
├── src/              # Source: main.cpp, hal.cpp, skill.cpp
├── lib/ArduClawLLM/  # LLM client library
├── ArduClaw-flasher/ # Web-based firmware flasher
├── test/
├── skills.md         # Full skill specification
├── platformio.ini    # PlatformIO config
└── build.ps1         # Build & merge script
```

### Supported Boards

| Board        | Status       |
| ------------ | ------------ |
| ESP32 DevKit | ✅            |
| ESP32-WROOM  | ✅            |
| ESP32-C3     | ✅            |
| ESP32-S2     | ⚠️           |
| ESP8266      | Experimental |

### Quick Start

1. Install [PlatformIO](https://platformio.org) in VS Code.
2. Open the project folder.
3. Build: `pio run`
4. Upload: `pio run --target upload`
5. Open Serial Monitor (115200 baud) and send JSON commands.

### Firmware Build

Run `build.ps1` to merge bootloader + partitions + firmware into one flashable binary for the browser flasher.

### Tech Stack

- **Framework:** Arduino Framework on ESP-IDF / FreeRTOS
- **Language:** C++17
- **IDE:** Visual Studio Code + PlatformIO
- **Web UI:** Vanilla HTML/CSS/JS (no frameworks)
- **Flasher:** Tailwind CSS, Web Serial API

---

</details>

<details>
<summary><b>Bahasa Indonesia</b></summary>

ArduClaw adalah runtime otomatisasi ringan untuk ESP32, terinspirasi dari konsep edge automation modern (ESP-Claw), tetapi dibuat lebih sederhana dan ringan. Dilengkapi dengan sistem skill berbasis event, integrasi AI/LLM, dukungan MQTT, dashboard web, dan firmware flasher dari browser.

### Fitur

- **Runtime Ringan** — ~50–120KB RAM, ~500KB–1.5MB flash. Tidak perlu PSRAM.
- **Sistem Skill Event-Driven** — Kontrol GPIO, ADC, PWM, I2C, SPI, UART via JSON tool calling.
- **Integrasi AI/LLM** — OpenAI, Gemini, OpenRouter, Ollama, atau API kompatibel OpenAI.
- **WiFi Manager** — Konfigurasi SSID/password tanpa recompile firmware.
- **Dukungan MQTT** — Publish, subscribe dengan wildcard matching dan dispatch aksi.
- **Web Dashboard** — Chat dengan AI, status perangkat, konfigurasi WiFi/LLM/MQTT, daftar skill.
- **Task Blink & Monitor** — Kedip GPIO non-blocking, pantau state pin dengan debounce.
- **Penyimpanan NVS** — Mode GPIO, aturan monitor, dan subscription MQTT awet setelah reboot.
- **Flasher Browser** — Upload firmware `.bin` via Web Serial API.
- **Arsitektur Multi-Task** — FreeRTOS dual-core: Core 0 (loop, skill, mqtt), Core 1 (wifi, llm).

### Struktur Proyek

```
ArduClaw/
├── include/          # Header: hal.h, skill.h, dashboard.h
├── src/              # Source: main.cpp, hal.cpp, skill.cpp
├── lib/ArduClawLLM/  # Library klien LLM
├── ArduClaw-flasher/ # Flasher firmware berbasis web
├── test/
├── skills.md         # Spesifikasi skill lengkap
├── platformio.ini    # Konfigurasi PlatformIO
└── build.ps1         # Script build & merge
```

### Board yang Didukung

| Board        | Status       |
| ------------ | ------------ |
| ESP32 DevKit | ✅            |
| ESP32-WROOM  | ✅            |
| ESP32-C3     | ✅            |
| ESP32-S2     | ⚠️           |
| ESP8266      | Eksperimental |

### Mulai Cepat

1. Install [PlatformIO](https://platformio.org) di VS Code.
2. Buka folder proyek.
3. Build: `pio run`
4. Upload: `pio run --target upload`
5. Buka Serial Monitor (115200 baud) dan kirim perintah JSON.

### Build Firmware

Jalankan `build.ps1` untuk menggabungkan bootloader + partitions + firmware menjadi satu binary yang siap di-flash dari browser.

### Tech Stack

- **Framework:** Arduino Framework di atas ESP-IDF / FreeRTOS
- **Bahasa:** C++17
- **IDE:** Visual Studio Code + PlatformIO
- **Web UI:** HTML/CSS/JS murni (tanpa framework)
- **Flasher:** Tailwind CSS, Web Serial API

---

</details>

---

## 📜 License

MIT

> **Small Devices, Smart Automation**
