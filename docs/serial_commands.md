# Serial Command Reference

Every command DRAC OS accepts over its USB serial console (115200 baud). This is a curated
transcription of the device's own `help` output (`src/utils/diagnostics.cpp`) — if it ever drifts
from what the device actually prints, trust the device and file a doc bug.

Requires a USB connection — see [Troubleshooting](troubleshooting.md) if commands don't respond.
For day-to-day use of the device itself (no serial required), see the [User Manual](manual.md).

---

## Core

| Command | Description |
|---|---|
| `help` | Show the full command list |
| `version` | Show firmware version |
| `serial on\|off` | Enable/disable all serial logging (input still works while off) |

---

## GPS

Module-aware: commands operate on whichever module is currently pinned (BH-880 speaks UBX; LC76G
and BN-880 speak NMEA/PAIR). See [`docs/bh880_module.md`](bh880_module.md) for the multi-module
design.

| Command | Description |
|---|---|
| `gps info` | Print chip SW/HW version (identifies the actual chip) |
| `gps ping` | Test TX/RX wiring — no fix needed |
| `gps status` | Show fix status, coordinates, satellites, HDOP |
| `gps quality` | Detailed fix-quality report |
| `gps raw [seconds]` | Dump raw protocol bytes (default 5s, max 30s) |
| `gps power full` | Full power mode (10Hz normal) |
| `gps power agg1` | Aggressive 1Hz low-power mode |
| `gps power interval [period_ms] [on_ms]` | Duty-cycle mode (default 10000/3000) |
| `gps config baud <rate>` | Change baudrate (9600–921600) |
| `gps restart hot\|warm\|cold` | Restart GPS (hot = fastest, cold = full search) |
| `gps reset` | Factory reset the GPS module (erases its saved settings) |
| `gps module` | Show the currently pinned GPS/compass module |
| `gps module set bh880\|lc76g\|bn880\|be881` | Pin a module directly, reboots to apply |
| `gps module reset` | Clear the pin so the first-boot picker shows again |

---

## GPX Web Server

| Command | Description |
|---|---|
| `gpx status` | Show GPX web server status, IP, connected clients |
| `gpx ap` | Switch to Access Point mode |
| `gpx sta` | Switch back to Station mode (join saved WiFi) |
| `gpx restart` | Restart the web server |
| `gpx index` | Show two-tier waypoint index stats (see [ADR-0023](adr/0023-two-tier-waypoint-index.md)) |
| `gpx index list [N]` | List the closest N working-set waypoints with distance |
| `gpx index reselect <lat> <lon>` | Debug: force a reselect against a synthetic center |
| `gpx index gentest <lat> <lon> [count]` | Debug: write `count` waypoints near center + reload |
| `gpx index genfiles <lat> <lon> [count]` | Debug: write `count` separate files (1 waypoint each) + timed reload |
| `gpx index genfiles clean` | Debug: remove `genfiles`' files + reload |

---

## Diagnostics

| Command | Description |
|---|---|
| `diag i2c` | Scan the I2C bus; print per-device stats (ops/fails/latency) |
| `diag wifi on\|off` | Enable/disable WiFi scanning |
| `diag ble on\|off` | Enable/disable Bluetooth scanning |
| `diag ap on\|off` | Enable/disable Access Point mode |
| `diag freeze on\|off` | Freeze/unfreeze the LVGL display (rendering stress test) |
| `diag touch on\|off` | Log raw + LVGL touch coordinates (off by default) |

---

## Tasks (FreeRTOS)

See [CLAUDE.md](../CLAUDE.md)'s Task Architecture section for what each task does.

| Command | Description |
|---|---|
| `task status` | Task status and health |
| `task stats` | Task performance statistics |
| `task enable` | Start FreeRTOS tasks |
| `task disable` | Stop tasks, fall back to legacy loop |
| `task control <ui> <i2c> <net> <sys>` | Enable/disable individual tasks (1/0 each) |

---

## Memory

| Command | Description |
|---|---|
| `memory` / `memory stats` | Current heap/PSRAM/LVGL usage |
| `memory info` | Memory layout info |
| `memory report` | Generate a full memory report |
| `memory pools` | Static pool usage |
| `memory cleanup [screens\|lvgl]` | Force cleanup |
| `memory integrity` | Heap integrity check |
| `memory leak start\|stop\|report` | Leak detection |
| `memory stress` | Run a memory stress test against the static pools |

---

## Configuration

| Command | Description |
|---|---|
| `config show` | All configuration values |
| `config display` | Display-specific parameters |
| `config timing` | Timing intervals and delays |
| `config pins` | GPIO pin assignments |
| `config set <param> <value>` | Set a runtime parameter (`wifi_interval`, `ble_interval`, `rtc_interval`, `brightness`) |

---

## Battery

| Command | Description |
|---|---|
| `battery` / `battery status` | Voltage, state, percentage |
| `battery voltage` | Battery voltage |
| `battery percent` | Battery percentage |
| `battery charging` | Charging yes/no |
| `battery state` | charging/discharging/full/stable |
| `battery raw` | Raw ADC value |
| `battery info` | Hardware configuration |
| `battery monitor on\|off` | Periodic logging |

Full guide: [`docs/battery_monitoring.md`](battery_monitoring.md).

---

## Crash Logging

| Command | Description |
|---|---|
| `crash dump` | Show the last crash dump |
| `crash clear` | Clear crash dump data |
| `crash info` | Crash logging system info |

---

## System Logger (SD card, dev-mode)

| Command | Description |
|---|---|
| `log status` | Logger status and statistics |
| `log list` | List all log files with sizes |
| `log rotate` | Force rotation to the next file |
| `log flush` | Force-flush the write buffer to file |
| `log size` | Total size of all logs |
| `log enable\|disable` | Enable/disable logging |

---

## NTP / Time

| Command | Description |
|---|---|
| `ntp status` | NTP sync status |
| `ntp sync` | Force an NTP time sync |
| `ntp settime YYYY-MM-DD HH:MM:SS` | Manually set the RTC |
| `ntp timezone <gmt> [dst]` | Set timezone, e.g. `ntp timezone -5 1` |

---

## Compass / Tilt (QMC5883L + QMI8658)

Bench-side tools for the compass tilt-compensation work — see
[`docs/compass_calibration_foundation.md`](compass_calibration_foundation.md) and
[`docs/compass_tilt_bench.md`](compass_tilt_bench.md).

| Command | Description |
|---|---|
| `compass` / `compass status` | Chip ID and configuration |
| `compass init` | Initialize sensor (continuous mode) |
| `compass read` | Read raw X/Y/Z and computed heading |
| `compass stream [seconds]` | Stream readings |
| `compass tiltbench` | One-shot accel+mag readout (WP-6 bench procedure) |
| `compass tilt` | Tilt-compensated heading status + live reading |
| `accel` / `accel status` | QMI8658 state and kill-switch |
| `accel read` | Read accel (and gyro if on) once |
| `accel on\|off` | Kill switch for all accel I2C traffic |
| `accel gyro on\|off` | Enable gyro in the burst (logging only) |
| `flog` / `flog status` | Field log state (bench testing) |
| `flog start <label>` | Start a sample (e.g. `flat360`, `phone360`) |
| `flog stop` | Stop and close the current sample |

---

## Beacon (BLE proximity)

Full guide: [`docs/beacon_proximity.md`](beacon_proximity.md).

| Command | Description |
|---|---|
| `beacon` / `beacon status` | Beacon proximity status |
| `beacon enable on\|off` | Enable/disable the feature |
| `beacon mac XX:XX:XX:XX:XX:XX` | Set target beacon MAC |
| `beacon power -XX` | Set measured power (dBm at 1m) |
| `beacon n X.X` | Set path loss exponent (2.0–4.0, typical 2.5 indoor) |
| `beacon test` | Force a scan and report results |
| `beacon scan` | List every visible BLE device |
| `beacon debug` | Print internal module state |
| `beacon zone` | Current zone and hysteresis state |
| `beacon trend` | Trend history and calculated RSSI slope |
| `beacon reset` | Reset all smoothing/trend state |

---

## Render Performance

Full guide: [`docs/performance_optimization_backlog.md`](performance_optimization_backlog.md), and
the Render Pipeline section of [CLAUDE.md](../CLAUDE.md).

| Command | Description |
|---|---|
| `perf` | Full per-stage render timing breakdown for the last radar frame |
| `rot` | Show the current rotation mode |
| `rot on\|off\|tiled` | Switch rotation mode (`tiled` is the production default) |

---

## Developer Mode

| Command | Description |
|---|---|
| `dev on\|off` | Enable/disable dev mode (persists across reboots) |
| `dev show\|hide` | Show/hide the DEV tab in Settings without touching the rest of dev mode |
| `dev status` | Current dev mode / DEV tab / logging status |

Dev mode turns on SD-card system logging, the battery-voltage debug readout, the DEV tab in
Settings (SD logging toggle + status, NTP status, live render-perf label, and buttons into the
Field Log and Tilt Bench bench-testing screens), and the `/logs` web management page (404s when
dev mode is off). **It's serial-only** — there is no touchscreen toggle, so `dev on` is the only
way in. It's aimed at builders debugging their own hardware, not day-to-day use — see
[the manual's Developer Mode section](manual.md#developer-mode) for what it unlocks.
