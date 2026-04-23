# AxG Haven — WiFi Tool V0.1

<div align="center">

```
 █████╗ ██╗  ██╗ ██████╗     ██╗  ██╗ █████╗ ██╗   ██╗███████╗███╗   ██╗
██╔══██╗╚██╗██╔╝██╔════╝     ██║  ██║██╔══██╗██║   ██║██╔════╝████╗  ██║
███████║ ╚███╔╝ ██║  ███╗    ███████║███████║██║   ██║█████╗  ██╔██╗ ██║
██╔══██║ ██╔██╗ ██║   ██║    ██╔══██║██╔══██║╚██╗ ██╔╝██╔══╝  ██║╚██╗██║
██║  ██║██╔╝ ██╗╚██████╔╝    ██║  ██║██║  ██║ ╚████╔╝ ███████╗██║ ╚████║
╚═╝  ╚═╝╚═╝  ╚═╝ ╚═════╝     ╚═╝  ╚═╝╚═╝  ╚═╝  ╚═══╝  ╚══════╝╚═╝  ╚═══╝
```

**WiFi Audit Tool V0.1**  
*AxG Haven-Labs | haven004*

![Platform](https://img.shields.io/badge/platform-ESP8266-blue?style=for-the-badge)
![Language](https://img.shields.io/badge/language-C%2B%2B%20%2F%20Arduino-orange?style=for-the-badge)
![Version](https://img.shields.io/badge/version-0.1-green?style=for-the-badge)
![License](https://img.shields.io/badge/license-MIT-yellow?style=for-the-badge)

> ⚠️ **For educational and authorized penetration testing use only.**  
> Unauthorized use against networks you don't own is illegal.

</div>

---

## 📋 Table of Contents

- [Overview](#-overview)
- [Hardware Requirements](#-hardware-requirements)
- [Pinouts](#-pinouts)
- [Features](#-features)
  - [Attack Tools](#️-attack-tools)
  - [Portal Themes](#-portal-themes)
  - [RF Radio Tools](#-rf-radio-tools-nrf24l01)
  - [Monitoring Tools](#-monitoring-tools)
  - [UI / UX](#️-ui--ux)
- [Setup & Installation](#-setup--installation)
- [Usage Guide](#️-usage-guide)
- [Results & Credential Dump](#-results--credential-dump)
- [Credits](#-credits)
- [License](#-license)
- [Disclaimer](#️-disclaimer)

---

## 🧠 Overview

**AxG Haven WiFi Audit Tool** is an ESP8266-based portable wireless auditing platform built by AxG Haven-Labs. It runs on a single NodeMCU/ESP8266 board with a 0.98" OLED display and provides a full menu-driven interface for WiFi auditing, RF monitoring, and network analysis.

Designed to be compact, battery-friendly, and operated entirely via 3 physical buttons — no phone, no laptop required in the field.

---

## 🔧 Hardware Requirements

| Component | Details |
|-----------|---------|
| **Microcontroller** | ESP8266 0.96" OLED Module V2.1.2 (built-in OLED) |
| **Display** | 0.96" OLED (onboard, no wiring needed) |
| **RF Module** | NRF24L01 (with SPI interface) |
| **Buttons** | 3× tactile buttons (UP / DOWN / SELECT) |
| **Power** | 3.3V regulated (LiPo + TP4056 recommended) |
| **Capacitor** | 10µF across NRF24L01 VCC/GND (recommended for stability) |

---

## 📌 Pinouts

### OLED Display (I2C)

| OLED Pin | ESP8266 GPIO | NodeMCU Label |
|----------|-------------|---------------|
| SDA | GPIO14 | D5 |
| SCL | GPIO12 | D6 |

### NRF24L01 (SPI)

| NRF24 Pin | ESP8266 GPIO | NodeMCU Label |
|-----------|-------------|---------------|
| CE | GPIO2 | D4 |
| CSN | GPIO4 | D2 |
| SCK | GPIO14 | D5 |
| MOSI | GPIO13 | D7 |
| MISO | GPIO12 | D6 |
| VCC | 3.3V | 3V3 |
| GND | GND | GND |

> ⚡ **Note:** SCK and SDA share GPIO14 (D5), SCL and MISO share GPIO12 (D6). `spiStop()` / `spiStart()` calls are used in firmware to safely switch the SPI bus between OLED and NRF24.

### Buttons (wire to GND, INPUT_PULLUP)

| Button | ESP8266 GPIO | NodeMCU Label |
|--------|-------------|---------------|
| UP | GPIO4 | D2 |
| DOWN | GPIO0 | D3 |
| SELECT | GPIO3 | RX |

---

## ✨ Features

### ⚔️ Attack Tools

| Feature | Description |
|---------|-------------|
| **WiFi Scan** | Scans for nearby networks with animated radar loading screen (~2.4s, 18 frames) and displays RSSI bars |
| **Deauth Attack** | Continuous 802.11 deauthentication / disassociation burst targeting a selected AP |
| **EvilTwin** | Clones the target SSID on the same channel, spawns a captive portal to capture credentials |
| **Combo Mode** | Runs Deauth + EvilTwin simultaneously |
| **EEPROM Save/Load** | Captured credentials persist across reboots via EEPROM storage |
| **Live Client Count** | Shows number of connected clients while EvilTwin is active |

---

### 🎭 Portal Themes

8 built-in captive portal themes:

| # | Theme | Type | Fields |
|---|-------|------|--------|
| 1 | **Router Repair** | Generic firmware-error page | Password only |
| 2 | **ISP Login** | PLDT / Globe / Sky portal | User + Password |
| 3 | **WiFi Login** | Generic network access | Password only |
| 4 | **Facebook** | Pixel-accurate Meta mobile login | User + Password |
| 5 | **Gmail** | Google account sign-in | User + Password |
| 6 | **Google WiFi** | Google network captive portal | Password only |
| 7 | **Jollibee Guest** | Jollibee free WiFi portal | Mobile + Password |
| 8 | **SM WiFi** | SM Supermalls SMAC portal | Email/SMAC + Password |

---

### 📡 RF Radio Tools (NRF24L01)

| Feature | Description |
|---------|-------------|
| **BLE Jammer** | Hops advertising channels 37/38/39 (NRF offsets 2/26/80) |
| **WiFi Jammer** | Hops all 11 WiFi channels (2412–2462 MHz offsets) |
| **BLE Scanner** | Passive scan on advertising channels 37/38/39 with animated pulse per channel |
| **BLE Monitor** | Live 3-panel scrolling waveform (ch37/38/39), RPD + packet hit count |

---

### 📊 Monitoring Tools

| Feature | Description |
|---------|-------------|
| **WiFi Channel Monitor** | Full-screen scrolling packet waveform with deauth overlay, ch 1–13, hop live |
| **Signal-Strength Bars** | 4-bar RSSI indicator in network list |
| **Scrolling SSID Ticker** | Auto-scrolls long SSIDs in the network list |
| **Status Screen** | Shows target, deauth/twin state, theme, jammer, and client count |

---

### 🖥️ UI / UX

- Scrollable **main menu** — 9 items with `^` / `v` scroll indicators
- Scrollable **theme menu** — 8 themes with scroll indicators
- Animated **radar sweep** for WiFi scan (~2.4s, 18 frames)
- Animated **BLE pulse** per channel (~2s, 8 frames × 3 channels)
- **Word-wrap helper** for long SSID / PASS / USER display on OLED
- **Direct launch attacks** — instant on SELECT
- All menus safely share SPI bus via `spiStop()` / `spiStart()`

---

## 🚀 Setup & Installation

### 1. Install Arduino IDE & ESP8266 Board Support

In Arduino IDE → **File → Preferences → Additional Board Manager URLs**, add:
```
http://arduino.esp8266.com/stable/package_esp8266com_index.json
```
Then go to **Tools → Board → Board Manager** and install **esp8266 by ESP8266 Community**.

### 2. Install Required Libraries

Install the following via **Sketch → Include Library → Manage Libraries**:

| Library | Purpose |
|---------|---------|
| `Adafruit SSD1306` | OLED display driver |
| `Adafruit GFX` | Graphics primitives |
| `RF24` | NRF24L01 radio driver |
| `ESP8266WiFi` | WiFi stack (built-in with ESP8266 core) |
| `DNSServer` | Captive portal DNS redirection |
| `ESP8266WebServer` | Serves the captive portal pages |
| `EEPROM` | Credential persistence (built-in) |

### 3. Flash the Firmware

1. Clone this repo or download `AxGFInal.ino`
2. Open in Arduino IDE
3. Select board: **NodeMCU 1.0 (ESP-12E Module)**
4. Set Upload Speed: **115200**
5. Select your COM port
6. Click **Upload**

---

## 🕹️ Usage Guide

### Button Layout

```
[ UP ]   [ DOWN ]   [ SELECT ]
  ↑          ↓         ✓/Enter
```

- **UP / DOWN** — scroll through menu items
- **SELECT** — enter submenu or launch feature

### Running an Attack

1. Go to **Attack Tools → WiFi Scan**
2. Wait for radar animation to complete
3. Scroll to target network → SELECT
4. Choose attack mode: **Deauth**, **EvilTwin**, or **Combo**
5. Choose portal theme (for EvilTwin / Combo)
6. Attack launches instantly

---

## 📂 Results & Credential Dump

Captured credentials are stored in **EEPROM** and survive power cycles.

1. Go to **Results** from main menu
2. Credentials shown on OLED (word-wrapped)
3. Connect serial monitor at **115200 baud** → select **DUMP** for full serial output

Serial dump format:
```
=== AxG Haven Credential Dump ===
SSID : TargetNetwork
USER : captured_user@example.com
PASS : capturedpassword
=================================
```

---

## 🙏 Credits

Big respect to the people whose work inspired and shaped this project:

| Person / Project | Contribution |
|-----------------|-------------|
| [**Spacehuhn** (SpacehuhnTech)](https://github.com/SpacehuhnTech) | Inspiration and concept for the WiFi channel monitor / scrolling packet waveform visualization |
| [**system-linux** — FazJammer](https://github.com/system-linux/FazJammer) | NRF24L01 BLE & WiFi jammer implementation concept and reference |
| **ESP8266_ZeroTwin** *(v1.0.0 alpha)* | EvilTwin captive portal concept and approach — no public repo found, credited by name |

> Thank you all for your contributions to the ESP8266 and wireless security community. This project wouldn't exist without your work. 🤝

---

## 📄 License

```
MIT License

Copyright (c) 2025 AxG Haven-Labs (haven004)

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

---

## ⚠️ Disclaimer

This tool is developed **strictly for educational purposes** and authorized security research.

- ✅ Use only on networks you own or have explicit written permission to test
- ❌ Unauthorized use is illegal under **RA 10175 (Philippines Cybercrime Prevention Act)**, the **Computer Fraud and Abuse Act (CFAA)**, and other applicable laws
- The authors of AxG Haven-Labs take **no responsibility** for misuse of this tool

**You are responsible for your own actions.**

---

<div align="center">

Made with ☕ by **AxG Haven-Labs** | haven004

*"Learn how things break so you can build them better."*

</div>
