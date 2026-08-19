# DRAC OS — GPS Radar for Waveshare ESP32-S3-Touch-LCD-2.1

[![PlatformIO](https://img.shields.io/badge/PlatformIO-Compatible-blue.svg)](https://platformio.org/)
[![License: CC BY-NC-SA 4.0](https://img.shields.io/badge/License-CC_BY--NC--SA_4.0-lightgrey.svg)](https://creativecommons.org/licenses/by-nc-sa/4.0/)
[![ESP32-S3](https://img.shields.io/badge/ESP32-S3-red.svg)](https://www.espressif.com/en/products/socs/esp32-s3)
[![LVGL](https://img.shields.io/badge/LVGL-8.4.0-green.svg)](https://lvgl.io/)
[![Powered by DRAC OS](https://img.shields.io/badge/Powered%20by-DRAC%20OS-orange.svg)](docs/manual.md)

DRAC OS is a self-contained GPS waypoint radar firmware for the Waveshare ESP32-S3-Touch-LCD-2.1
round display, part of the db-radar project. Load a GPX file over WiFi, step outside, and the radar
shows where each waypoint sits relative to the direction you're facing — no phone, no app, no signal
required once it's loaded.

---

## What It Does

**The radar** runs entirely on the device, powered by open-source DRAC OS firmware. Waypoints appear as dots on a compass-driven radar that
rotates as you turn (or stays fixed north-up, your choice), with off-screen waypoints clustered
into edge indicators so a hundred-pin GPX file doesn't turn into visual noise. Fix a single
waypoint to get a live distance readout and a buzzer that quickens as you close in. A separate BLE
mode does the same thing for a Bluetooth tag instead of a GPS point — useful as a short-range item
finder.

### Features

- **Compass-driven heading-up navigation** — the radar rotates as you turn; switch to a fixed
  north-up mode in Settings, persisted across reboots (BH-880/BE-881/BN-880 builds only — an LC76G
  build has no compass and runs North-Up automatically, see [GPS Module Options](#gps-module-options))
- **GPS waypoint radar** — five zoom levels from 1km down to a 50m precision mode; your position
  is a red triangle at center, waypoints are yellow dots
- **Off-screen indicators** — waypoints outside the current zoom radius collapse into up to 8 edge
  arrows, one per compass sector, so dozens of distant pins don't clutter the screen
- **Waypoint fix mode** — single out a waypoint for a continuously-updating distance readout and a
  proximity sonar (buzzer tempo speeds up continuously as you approach, not in discrete zones);
  auto-releases if you end up more than 100km away
- **BLE beacon proximity** (50m zoom) — point the radar at a specific Bluetooth tag instead of a
  waypoint; an arc gauge and buzzer tone track signal strength continuously as you get closer
- **GPX file management over WiFi** — upload, browse, and delete `.gpx` files from any browser,
  no companion app or cable needed after initial setup
- **Two WiFi modes** — the device can host its own access point (no router needed) or join your
  home network
- **Over-the-air firmware updates** — flash new firmware from the browser once the device is on
  your network
- **Battery monitoring** — charge-level icon on-screen, colored by threshold
- **Daylight mode** — high-contrast black-on-white theme for outdoor visibility in direct sun
- **Hardware watchdog and crash logging** for field reliability

### In progress

**Quests** — a GPX file tagged as a set of waypoints to find as a group, with progress tracking
and a small collectible badge on completion. Design is substantially resolved and some prep work
has already shipped, but the feature itself isn't built yet — see
[docs/quests_plan.md](docs/quests_plan.md) and [ROADMAP.md](ROADMAP.md) for where it stands.

---

## Bill of Materials

One firmware image supports four GPS/compass modules — pick one. See
[GPS Module Options](#gps-module-options) below for how to choose.

| # | Part | Notes |
|---|------|-------|
| 1 | [Waveshare ESP32-S3-Touch-LCD-2.1](https://www.waveshare.com/esp32-s3-touch-lcd-2.1.htm) | Main board — 16MB Flash, 8MB PSRAM, 480×480 round display — [wiki](https://docs.waveshare.com/ESP32-S3-Touch-LCD-2.1) |
| 2 | [Beitian BH-880](https://www.beitian.com/en/sys-pd/1871.html) (primary), BE-881, LC76G, **or** BN-880 | GPS module — BH-880/BE-881/BN-880 each add a built-in compass; LC76G doesn't (device runs North-Up automatically) |
| 3 | 3.7V 1S LiPo, 1000–2000mAh | Any 1S LiPo in that range works |
| 4 | MT3608 boost converter | Steps the board's 3.3V rail up to 5V — required for modules needing 3.6–5.5V (see [Assembly Instructions](docs/assembly.md)) |
| 5 | 1.25mm 6-pin JST pigtail | Comes with the BH-880/BE-881 modules (LC76G/BN-880 typically use a smaller 4-pin JST instead — check yours) |
| 6 | 3D printed enclosure | See [3D Enclosure](#3d-enclosure) below |
| 7 | M2 screws | Enclosure mounting |
| 8 | M1.4 screws | Fixing components inside the enclosure |

---

## GPS Module Options

One firmware image, four supported modules — pick based on whether you want the compass, and
which one you can source:

- **BH-880** (primary, recommended) — GPS + compass (QMC5883L) in one module, enables Heading-Up
  navigation (the radar rotates as you turn). Needs 3.6–5.5V, so it's powered through a boost
  converter (see [Assembly Instructions](docs/assembly.md)).
- **BE-881** — GPS + compass (QMC5883P), same UBX protocol family as the BH-880.
- **LC76G** — GPS only, no compass. Cheaper and simpler to wire (no I2C connection needed), but the
  device is locked to North-Up navigation since there's no heading source to rotate by — the
  Settings screen disables the Heading-Up option automatically rather than offering one that would
  silently do nothing.
- **BN-880** — GPS + compass (HMC5883L), NMEA/PAIR protocol family (same path as LC76G). Fully
  supported in firmware but not yet on the first-boot picker pending further field verification —
  reachable via Settings or serial (`gps module set bn880`).

BH-880, BE-881, and LC76G are the three field-verified options offered on the first-boot picker. The
module type is a one-time choice: the picker appears on first boot, or set it any time via
**Settings > GPS** or serial (`gps module set bh880|lc76g|bn880|be881`, see
[docs/serial_commands.md](docs/serial_commands.md)). Full detail on all four, including the UBX vs.
NMEA protocol handling under the hood: [docs/bh880_module.md](docs/bh880_module.md).

---

## 3D Enclosure

A custom enclosure designed for this build is available at:

[**Dragon Ball Radar**](https://makerworld.com/en/models/2776400-dragon-ball-radar)

**Print settings**: PLA or PETG, 0.2mm layer height, 20% infill. Mounts the board face-up, with a
cutout for the USB-C port and a recess for the LiPo underneath.

---

## Installation

Three ways to get firmware onto the board — you don't need any toolchain unless you want one.

### Option A — Browser Web Flasher (easiest, no install required)

1. Open the web flasher: **[alvroga.github.io/db-radar/flasher](https://alvroga.github.io/db-radar/flasher/)**
   (Chrome or Edge on desktop — Web Serial required)
2. Plug the board in via a USB-C **data** cable (charge-only cables won't work)
3. Click **Connect & Install**, select the serial port, and choose **Erase device and install** on
   a new board

### Option B — Download + esptool

1. Download the latest release from the [Releases page](https://github.com/alvroga/db-radar/releases/latest)
2. Grab the full merged image (bootloader + partition table + app, one file)
3. Flash it to a blank board:
   ```bash
   pip install esptool
   esptool.py --chip esp32s3 write_flash 0x0 <downloaded-file>.bin
   ```

### Option C — Build from source with PlatformIO

```bash
git clone https://github.com/alvroga/db-radar.git
cd db-radar
pio run -e db-radar -t upload
```

Already flashed and just want to update? Use the device's own web portal instead — open
`http://<device-ip>/update` and upload the smaller, app-only OTA image from the same release (not
the full image — the portal expects the raw app binary only).

### First Boot

1. Wire the GPS module per the [Assembly Instructions](docs/assembly.md)
2. Display shows the green radar background
3. GPS status reads "Waiting for fix..." until satellite lock (cold start ~30s outdoors)
4. After fix: waypoints appear, compass drives radar rotation

### Loading GPX Waypoints

1. Power on the board — it creates a WiFi access point named `Radar-GPX` (password: `radar123`)
2. Connect your phone or laptop to that network
3. Browse to `http://192.168.4.1`
4. Upload any `.gpx` file — waypoints appear on the radar immediately

Alternatively, switch to STA mode in Settings to join your home network instead; the web portal is
then reachable at the device's IP address, shown on the Settings screen.

Don't have a GPX file yet? Build one — including per-waypoint hint text — with the
**[GPX Generator](https://alvroga.github.io/db-radar/gpx-generator/)**, or export one from any standard
geocaching/GPX tool; the radar reads the `<groundspeak:encoded_hints>` field automatically if present.

---

## Architecture

### Software Structure

```
src/
├── core/
│   ├── main.cpp                  # Entry point
│   ├── device_manager.cpp        # Hardware init, LVGL setup
│   ├── ui_manager.cpp            # Screen creation, widget management
│   ├── navigation.cpp            # Radar drawing, event handling
│   └── diagnostics.cpp           # Serial command interface
├── hardware/
│   ├── display/                  # ST7701, CST820, backlight drivers
│   ├── sensors/                  # RTC, GPS (BH-880/BE-881/LC76G/BN-880), compass, battery
│   ├── i2c/                      # Unified I2C manager with retry logic
│   ├── connectivity/             # WiFi/BLE scanner, beacon proximity
│   └── buzzer.cpp                # Buzzer control via TCA9554
├── ui/
│   ├── settings_screen.cpp       # Settings UI
│   ├── waypoint_screen.cpp       # Waypoint detail view
│   └── dev_screen.cpp            # Dev-mode diagnostic overlay
├── utils/
│   ├── task_manager.cpp          # FreeRTOS 4-task architecture
│   ├── settings_manager.cpp      # NVS read/write
│   ├── wmm_declination.cpp       # WMM magnetic declination model
│   └── watchdog.cpp              # Hardware watchdog
└── gpx/                          # GPX parser, loader, and two-tier waypoint index
```

### FreeRTOS Task Architecture

| Task | Core | Priority | Loop | Responsibility |
|------|------|----------|------|----------------|
| UI Task | 1 | 5 | 10ms | LVGL rendering, touch/button input, radar draw |
| I2C Task | 0 | 2 | 20ms | Queued I2C device communication (RTC, IO expander) |
| Network Task | 0 | 1 | 200ms | WiFi/BLE scanning |
| System Task | 0 | 1 | 100ms | GPS parsing, compass reads (10Hz), battery, health checks |

The UI task is alone on core 1 so touch and rendering never wait on I2C or radio work; everything
else shares core 0 through queued requests rather than direct calls.

### Rendering

The radar isn't drawn to an offscreen canvas and copied — it paints directly into LVGL's draw
context from a draw-event callback, so there's no intermediate buffer or frame copy. The physical
90° enclosure rotation is handled by a tiled transpose in the display flush callback (not LVGL's
software rotation), writing straight into the panel's back framebuffer. Frame time is roughly 85ms
at 240MHz (about 10-12 fps) — plenty for a radar display where the picture only needs to update as
fast as you can walk or turn.

### Driver Table

| Component | Driver | Status |
|-----------|--------|--------|
| Display | ST7701, 480×480 @ 10MHz PCLK | Production |
| Touch | CST820 | Production |
| Backlight | PWM, GPIO6 | Production |
| RTC | PCF85063 | Production |
| IO Expander | TCA9554 | Production |
| GPS | BH-880/BE-881 (UBX protocol) or LC76G/BN-880 (NMEA/PAIR) — one pinned choice, see [GPS Module Options](#gps-module-options) | Working |
| Compass | QMC5883L (BH-880), QMC5883P (BE-881), or HMC5883L (BN-880), tilt-compensated + WMM declination; LC76G has none, forced North-Up | Working |

---

## Serial Diagnostic Commands

Open a serial monitor at 115200 baud (requires a USB connection) and type `help` for the full list.
Highlights:

```
memory stats            Current heap/PSRAM/LVGL usage
task status             FreeRTOS task statistics and health
battery status          Battery percentage and trend
beacon status           BLE beacon scan state and RSSI
config show             All configuration values
diag wifi on|off        Enable/disable WiFi scanning
diag ble on|off         Enable/disable BLE scanning
```

---

## Build Metrics

Measured directly from a clean build:

- **RAM**: 51.4% (168,352 / 327,680 bytes)
- **Flash**: 41.0% (1,718,851 / 4,194,304 bytes — one OTA app partition; the board's flash is 16MB
  total, split across two OTA slots plus onboard storage)
- **PSRAM**: 8MB octal PSRAM handles the LVGL display buffers and the full-device waypoint index;
  most of it stays free for application use

---

## Documentation

- **[docs/manual.md](docs/manual.md)** — User manual: every mode, every setting, waypoints and
  hints, BLE beacon tracking, WiFi/GPX management, assembly steps, and what's coming next
- **[docs/assembly.md](docs/assembly.md)** — Wiring diagrams and build photos (in progress)
- **[docs/serial_commands.md](docs/serial_commands.md)** — Full serial console command reference
- **[docs/bh880_module.md](docs/bh880_module.md)** — GPS module support: BH-880, BE-881, LC76G, and
  BN-880, protocol handling, module pinning
- **[CLAUDE.md](CLAUDE.md)** — Full technical reference: architecture, configuration, hardware
  integration, every subsystem in detail
- **[CHANGELOG.md](CHANGELOG.md)** — Complete implementation history
- **[ROADMAP.md](ROADMAP.md)** — Known issues and planned work, including Quests
- **[docs/waypoint_filtering.md](docs/waypoint_filtering.md)** — Waypoint filtering and off-screen indicators
- **[docs/navigation_modes.md](docs/navigation_modes.md)** — Heading-up vs. north-up navigation
- **[docs/beacon_proximity.md](docs/beacon_proximity.md)** — BLE beacon proximity system
- **[docs/battery_monitoring.md](docs/battery_monitoring.md)** — Battery ADC and display
- **[docs/standby_mode.md](docs/standby_mode.md)** — Low-power standby mode
- **[docs/firmware_installation.md](docs/firmware_installation.md)** — Release pipeline and web flasher internals
- **[docs/troubleshooting.md](docs/troubleshooting.md)** — Common issues and solutions

---

## Support

This project is free and will stay free. If it's useful to you and you'd like to support ongoing
development, a donation is welcome and never required:

> **[Buy Me a Coffee / donation link goes here]**

---

## Contributing

Not actively seeking pull requests, but GitHub Issues for bug reports and hardware compatibility
notes are welcome.

---

## License

[Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International (CC BY-NC-SA 4.0)](https://creativecommons.org/licenses/by-nc-sa/4.0/)

Copyright (c) 2025–2026 Alvaro Robles

You're free to share and adapt this work for non-commercial purposes, provided you give appropriate
credit and distribute any derivatives under the same license. Commercial use requires a separate
license from the author.

This material is provided **as-is**, with no warranties. The author is not liable for any damages,
hardware failures, or losses arising from building or using this project. Build and use at your own
risk.

---

## Acknowledgments

- **Waveshare** — ESP32-S3-Touch-LCD-2.1 hardware
- **LVGL** — Embedded graphics library
- **Espressif** — ESP32-S3 and ESP-IDF
- **PlatformIO** — Embedded development platform
- **h2zero/NimBLE-Arduino** — Lightweight BLE stack
- **Beitian** — BH-880 GPS + compass module
- **Quectel** — LC76G GNSS chip (used in third-party LC76G breakout modules)

---

**Board**: Waveshare ESP32-S3-Touch-LCD-2.1 (16MB Flash, 8MB PSRAM)
**Firmware**: Powered by [DRAC OS](docs/manual.md) — free, open-source, flashed from your browser.
