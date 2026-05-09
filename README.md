# cc-radar — GPS Radar for Waveshare ESP32-S3-Touch-LCD-2.1

[![PlatformIO](https://img.shields.io/badge/PlatformIO-Compatible-blue.svg)](https://platformio.org/)
[![License: CC BY-NC-SA 4.0](https://img.shields.io/badge/License-CC_BY--NC--SA_4.0-lightgrey.svg)](https://creativecommons.org/licenses/by-nc-sa/4.0/)
[![ESP32-S3](https://img.shields.io/badge/ESP32-S3-red.svg)](https://www.espressif.com/en/products/socs/esp32-s3)
[![LVGL](https://img.shields.io/badge/LVGL-8.3.11-green.svg)](https://lvgl.io/)

A GPS waypoint radar application for the Waveshare ESP32-S3-Touch-LCD-2.1 round touch display. Shows your position and saved waypoints on a live compass-driven radar, with GPX file management over WiFi, BLE beacon proximity detection, and robust field navigation features.

---

## 🎯 What Is This?

**cc-radar** is a self-contained GPS navigation tool built on the ESP32-S3. It runs entirely on the device — no phone required. Load GPX waypoints over WiFi, step outside, and the radar shows where each waypoint is relative to where you're facing.

### Features

- 🧭 **Compass-driven heading-up navigation** — radar rotates with you, north indicator shows true north
- 🗺️ **North-up mode** — classic fixed-north orientation, persisted in NVS
- 📍 **Real-time GPS waypoint radar** — yellow dots on green background with black grid
- 🔴 **Position indicator** — red triangle at center marking your location
- 📏 **5 zoom levels** — 50m / 100m / 200m / 500m / 1km (default 100m)
- 🟠 **Off-screen indicators** — orange arrows at screen edge, max 8 (one per compass sector)
- 🗂️ **Waypoint management** — long-press to add, tap to view/delete, fix a waypoint for proximity nav
- 📌 **Fixed waypoint mode** — single-dot focus, live distance label ("Fixed: 23m"), proximity sonar via buzzer, star appears when close, auto-unfixes beyond 1km
- 🔵 **BLE beacon proximity** (50m zoom only) — 4-zone ring gauge (VERY_FAR/FAR/MEDIUM/CLOSE), sonar tempo speeds up, found indicator persisted in NVS
- 📂 **GPX file management** — upload/view/delete `.gpx` files from any browser via the web portal
- 📡 **WiFi AP boot mode** — device creates its own access point (`Radar-GPX`); no router required
- 🌐 **WiFi STA boot mode** — device connects to a network on boot; web portal accessible by IP
- 🔄 **OTA firmware update** — flash new firmware from the browser at `/update`
- 🔖 **CalVer versioning** — `vYY.MM.DD` build stamp displayed on loading screen
- 🔋 **Battery monitoring** — GPIO4 ADC, battery symbol icon (full/3-bar/2-bar/1-bar/empty/charging)
- ☀️ **Daylight mode** — high-contrast black-on-white theme for outdoor use
- ⚙️ **Settings screen** — GPS, WiFi, Display, Sound, Beacon, Dev tabs; all settings NVS-persisted
- 🛡️ **Hardware watchdog** + crash logging to RTC memory
- 🖥️ **DEV mode serial commands** — memory, task, beacon, battery, config diagnostics

---

## 🖼️ Gallery

| Radar Screen | Waypoint List | Settings |
|:---:|:---:|:---:|
| *(photo)* | *(photo)* | *(photo)* |

---

## 🔩 Bill of Materials

| # | Part | Notes |
|---|------|-------|
| 1 | [Waveshare ESP32-S3-Touch-LCD-2.1](https://www.waveshare.com/esp32-s3-touch-lcd-2.1.htm) | Main board — 16MB Flash, 8MB PSRAM, 480×480 round display — [wiki](https://docs.waveshare.com/ESP32-S3-Touch-LCD-2.1) |
| 2 | [Beitian BH-880](https://www.beitian.com/en/sys-pd/1871.html) | GPS (B1301N) + Compass (QMC5883L) — 1.25mm 6-pin connector |
| 3 | MakerHawk 3.7V 1500mAh LiPo | Any 1S LiPo 1000–2000mAh works |
| 4 | MT3608 3.7V → 5V boost converter | Steps board 3.3V up to 5V for the GPS module |
| 5 | 1.25mm 6-pin JST pigtail | BH-880 connector — comes with the module |
| 6 | 3D printed enclosure | See [3D Enclosure](#%EF%B8%8F-3d-enclosure) below |
| 7 | M2 screws | For mounting the enclosure |
| 8 | M1.4 screws | For fixing components inside the enclosure |

---

## 🔌 Wiring

### BH-880 → ESP32-S3 Board

The BH-880 has a 1.25mm 6-pin connector labeled `D G T R V C` (left to right).

| Pin | Label | Connect to | Function |
|-----|-------|------------|----------|
| 1 | D | GPIO 15 | I2C SDA (compass) |
| 2 | G | Boost converter GND out | Ground |
| 3 | T | GPIO 44 | GPS TX → board RX |
| 4 | R | GPIO 43 | GPS RX → board TX |
| 5 | V | Boost converter 5V out | Power |
| 6 | C | GPIO 7 | I2C SCL (compass) |

### Power

```
LiPo ──► Board BAT connector

Board 12-pin interface
  3.3V ──► Boost converter IN+
  GND  ──► Boost converter IN−
           Boost converter OUT+ (5V) ──► BH-880 pin V
           Boost converter OUT− (GND) ──► BH-880 pin G
```

The battery connects to the board's BAT connector — the board handles charging via USB-C. The board's 3.3V regulated output (available on the 12-pin interface) feeds the boost converter, which steps it up to 5V for the BH-880 GPS module. See the [board wiki](https://docs.waveshare.com/ESP32-S3-Touch-LCD-2.1) for the full 12-pin interface pinout.

---

## 🖨️ 3D Enclosure

A custom enclosure designed for this build is available at:

> **[3D Model — Printables / Thingiverse link coming soon]**

**Print settings**: PLA or PETG, 0.2mm layer height, 20% infill. The enclosure mounts the Waveshare board face-up and has a cutout for the USB-C port and a recess for the LiPo battery underneath.

---

## ⚡ Quick Start

### Requirements

- USB-C cable (data capable — charge-only cables won't work for upload)
- VSCode + [PlatformIO IDE extension](https://platformio.org/install/ide?install=vscode)
- All parts from the [Bill of Materials](#bill-of-materials)

### Build and Upload

```bash
# Clone the repository
git clone https://github.com/alvroga/db-radar.git
cd db-radar

# Open in VSCode, then use PlatformIO: Build + Upload
# Or from the terminal:
pio run -t upload

# Open serial monitor (115200 baud)
pio device monitor
```

### First Boot

1. Wire GPS module per the [Wiring](#-wiring) table above
2. Connect board via USB-C and upload firmware
3. Display shows green radar background
4. GPS status: "Waiting for fix..." until satellite lock (cold start ~30s outdoors)
5. After fix: waypoints appear, compass drives radar rotation

### Loading GPX Waypoints

1. Power on the board — it creates a WiFi AP named `Radar-GPX` (password: `radar123`)
2. Connect your phone or laptop to that network
3. Browse to `http://192.168.4.1`
4. Upload any `.gpx` file — waypoints appear immediately on the radar

Alternatively, configure STA mode in Settings to join your home network; the web portal is then accessible by the device's IP address shown on the settings screen.

---

## 📋 Architecture

### Software Structure

```
src/
├── core/
│   ├── main.cpp                  # 80-line entry point
│   ├── device_manager.cpp        # Hardware init, LVGL setup
│   ├── ui_manager.cpp            # Screen creation, widget management
│   ├── navigation.cpp            # Radar drawing, event handling
│   └── diagnostics.cpp           # Serial command interface
├── hardware/
│   ├── display/                  # ST7701, CST820, backlight drivers
│   ├── sensors/                  # RTC, GPS (BH-880), compass (QMC5883L), battery
│   ├── i2c/                      # Unified I2C manager with retry logic
│   ├── connectivity/             # WiFi/BLE scanner, beacon proximity
│   └── buzzer.cpp                # Buzzer control via TCA9554
├── ui/
│   ├── settings_screen.cpp       # Settings UI (6 tabs)
│   ├── waypoint_screen.cpp       # Waypoint detail view
│   └── dev_screen.cpp            # DEV mode overlay
├── utils/
│   ├── task_manager.cpp          # FreeRTOS 4-task architecture
│   ├── power_manager.cpp         # Display and system power
│   ├── settings_manager.cpp      # NVS read/write
│   ├── wmm_declination.cpp       # WMM2020 magnetic declination model
│   └── watchdog.cpp              # Hardware watchdog
└── gpx/                          # GPX parser and loader
```

### FreeRTOS Task Architecture

| Task | Core | Priority | Loop | Responsibility |
|------|------|----------|------|----------------|
| UI Task | 1 | 3 | 10ms | LVGL, touch, button polling, radar draw |
| I2C Task | 0 | 2 | 20ms | RTC reads via queue |
| Network Task | 0 | 1 | 200ms | WiFi/BLE scanning |
| System Task | 0 | 1 | 200ms | GPS, battery, compass read, health checks |

### Driver Table

| Component | Driver | Status |
|-----------|--------|--------|
| Display | ST7701, 480×480 @ 10MHz PCLK | Production |
| Touch | CST820 | Production |
| Backlight | PWM GPIO6 | Production |
| RTC | PCF85063 | Production |
| IO Expander | TCA9554 | Production |
| GPS | BH-880 (B1301N UBX) | Working |
| Compass | QMC5883L + WMM declination | Working |

### Display Configuration

- **Full-frame LVGL buffers**: 480 lines (921KB PSRAM, 2 buffers)
- **1 flush per frame** — eliminates screen wipe artifact during transitions
- **Partial refresh**: only changed areas redrawn
- **Software rotation**: 90° CW to compensate physical enclosure orientation

---

## 🔧 Serial Diagnostic Commands

Open serial monitor at 115200 baud (requires USB connection) and type:

```
help                    Show all available commands
```

### Memory
```
memory stats            Current heap/PSRAM/LVGL usage
memory report           Comprehensive system report
memory integrity        Check heap integrity
memory leak start/stop/report  Leak detection
```

### Task Management
```
task status             Task statistics and health
task enable <task> on|off
```

### GPS & Sensors
```
battery status          Battery percentage and trend
battery raw             Raw ADC reading
beacon status           BLE beacon scan state and RSSI
beacon scan             Force immediate scan
beacon zone             Current proximity zone
beacon trend            RSSI trend over last 10 samples
```

### Configuration
```
config show             All configuration values
config display          Display parameters
config timing           Timing intervals
config pins             GPIO assignments
```

### Diagnostics
```
diag wifi on|off        Enable/disable WiFi scanning
diag ble on|off         Enable/disable BLE scanning
diag freeze on|off      Freeze/unfreeze LVGL (testing)
```

---

## 🛡️ Stability

- **Zero UI freezes** — queue-based I2C eliminates blocking operations on the UI core
- **Thread-safe LVGL** — mutex protection, all UI calls from UI Task only
- **Hardware watchdog** — TWDT detects and recovers hung tasks
- **Crash logging** — RTC memory captures crash state for post-mortem
- **Stable display timing** — 10MHz PCLK proven through extensive field testing
- **NimBLE** — ~25KB SRAM (vs ~65KB Bluedroid), 40KB headroom freed

### Build Metrics
- **RAM**: 58.9% (193KB / 327KB)
- **Flash**: 51.6% (1.62MB / 3.14MB)
- **PSRAM**: ~921KB LVGL buffers, ~85% free for application

---

## 📚 Documentation

- **[CLAUDE.md](CLAUDE.md)** — Complete technical reference (architecture, APIs, configuration)
- **[CHANGELOG.md](CHANGELOG.md)** — Full implementation history
- **[ROADMAP.md](ROADMAP.md)** — Known issues and planned work
- **[docs/waypoint_filtering.md](docs/waypoint_filtering.md)** — Waypoint filtering and off-screen indicators
- **[docs/navigation_modes.md](docs/navigation_modes.md)** — Heading-up vs north-up navigation
- **[docs/beacon_proximity.md](docs/beacon_proximity.md)** — BLE beacon proximity system
- **[docs/battery_monitoring.md](docs/battery_monitoring.md)** — Battery ADC and display
- **[docs/compass_i2c_constraint.md](docs/compass_i2c_constraint.md)** — Why compass reads from System Task
- **[docs/troubleshooting.md](docs/troubleshooting.md)** — Common issues and solutions

---

## 🤝 Contributing

This project is not actively seeking pull requests, but GitHub Issues for bug reports and hardware compatibility notes are welcome.

---

## 📄 License

[Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International (CC BY-NC-SA 4.0)](https://creativecommons.org/licenses/by-nc-sa/4.0/)

Copyright (c) 2025–2026 Alvaro Robles

You are free to share and adapt this work for non-commercial purposes, provided you give appropriate credit and distribute any derivatives under the same license.

This material is provided **as-is** with no warranties. The author is not liable for any damages, hardware failures, or losses arising from the use of this project. Build and use at your own risk.

---

## 🙏 Acknowledgments

- **Waveshare** — ESP32-S3-Touch-LCD-2.1 hardware
- **LVGL** — Embedded graphics library
- **Espressif** — ESP32-S3 and Arduino/ESP-IDF framework
- **PlatformIO** — Embedded development platform
- **h2zero/NimBLE-Arduino** — Lightweight BLE stack
- **Beitian** — BH-880 GPS + compass module

---

**Board**: Waveshare ESP32-S3-Touch-LCD-2.1 (16MB Flash, 8MB PSRAM)
