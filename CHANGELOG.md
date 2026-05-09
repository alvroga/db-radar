# Changelog - cc-radar Development History

All notable completed features and changes to the GPS Radar project.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

---

## [Beta] - 2026-04-13

### Added

**OTA Firmware Update via Web Browser**

Full over-the-air firmware update system accessible from the GPX web portal at `/update`. The device writes the new binary directly to the inactive OTA partition, sets the boot partition, and reboots. A one-shot server guard (`ota_already_triggered`) prevents a browser retry or stale tab from re-flashing the device after a successful update. The OTA page matches the dark monospace aesthetic of the rest of the portal.

- Dual OTA partition table (`partitions_ota.csv`): 2×2MB app slots + 11.7MB FFat
- `esp_ota_mark_app_valid_cancel_rollback()` called on successful boot (rollback safety)
- `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` added to `sdkconfig.defaults`
- Warning message on upload page: display interference during flash is expected
- Files: `src/gpx/gpx_server.cpp`, `partitions/partitions_ota.csv`, `sdkconfig.defaults`

**CalVer Versioning + Per-Build Stamp**

Build version `vYY.MM.DD` written to `include/core/fw_version_gen.h` by `scripts/gen_version.py` (PlatformIO pre-build extra_script). A Unix build timestamp `FW_BUILD_TS` is also written on every build, ensuring `settings_manager.cpp` always recompiles and `FW_STAMP_VAL` is unique per build.

- Version displayed on loading screen (radar green, `LV_ALIGN_BOTTOM_MID`)
- `version` serial command added to diagnostics
- Files: `scripts/gen_version.py`, `include/core/fw_version_gen.h`, `src/utils/settings_manager.cpp`

**Radar-Mode Boot Guarantee After Any Firmware Flash**

First boot after any firmware update (USB or OTA) now always lands in radar mode, regardless of previous WiFi settings.

- Root cause: `FW_STAMP_VAL` was `fnv1a(FW_VERSION)` — same-day builds produced identical stamps, NVS WiFi flags survived any same-day flash
- Fix: `FW_STAMP_VAL = FW_BUILD_TS` (Unix timestamp, unique per build). Any mismatch → WiFi boot flags cleared → radar mode
- OTA handler calls `settings_manager::prepareForOTAReboot()`: writes `fw_stamp=0` + clears `wifi_ap_en`/`wifi_sta_en` in one NVS transaction — stamp=0 always mismatches any real `FW_BUILD_TS`

### Changed

**GPX Web Portal — Dark Monospace Theme**

Main GPX upload page (`/`) redesigned to match the OTA page aesthetic: `#1a1a1a` dark background, monospace font, `#00ff00` green accents, dashed drag-drop zone, dark file list. Removed purple gradient, emojis, and white card. All JavaScript functionality (drag-and-drop, file list, download, delete) unchanged.

---

## [Beta] - 2026-03-25

### Added

**Beacon Found Indicator**

A 40×40 LVGL canvas overlay (circle + star) is drawn at screen center when the beacon has been marked as found (NVS `bcn_found` flag set). The indicator renders in white on the dark theme and black on daylight mode, matches the existing DEV label position, and hides automatically when the HUD is hidden.

**Beacon 4-Zone Musical Tempo**

Sonar beep intervals restructured to four distinct zones: VERY_FAR 1500ms, FAR 750ms, MEDIUM 500ms, CLOSE 250ms. EMA smoothing (α=0.4), ±3 dBm hysteresis between zone transitions, and trend detection over 10 samples prevent zone flicker. Replaces the previous linear interval mapping that produced choppy audio at close range (FT-04 resolved).

**Fixed Waypoint UX Improvements**

Proximity star drawn on the waypoint dot itself at three sizes scaled to zone distance. Waypoint label renamed to "Fixed:" with position tuned for safe screen margin. Auto-unfix triggers when distance exceeds 1km. North indicator is hidden when north-up mode is active (not needed when north is always up).

### Changed

**Beacon Scan Interval**

BLE scan interval reduced from 1000ms to 500ms, improving RSSI update responsiveness when near the target beacon.

**Beacon Found = Silent**

`updateSonar()` returns immediately when `g_found == true`. Prevents a NVS-persisted found state (from a previous session) from triggering audio at startup before the user resets the found flag.

**DEV Label Position**

DEV mode overlay label moved to screen center to avoid collision with other HUD elements at screen edges.

### Fixed

**WiFi AP Chunked HTTP Responses**

GPX management web page responses now sent in 1KB chunks via `httpd_resp_send_chunk()`. Fixes EAGAIN errors on the ESP32 soft-AP TCP buffer that caused partial page loads or browser timeouts when serving the file management UI.

---

## [Unreleased] - 2026-03-20

### Changed

**SRAM Optimization — NimBLE Migration**

Replaced Bluedroid BLE stack with NimBLE (`h2zero/NimBLE-Arduino@^1.4.0`) to resolve critical SRAM exhaustion caused by the Bluedroid stack consuming ~65KB at runtime.

**Root cause**: After BLE init, only ~2–7KB SRAM remained — not enough for SD card DMA buffers (~4–16KB). Every SD log flush failed with `sdmmc_read_blocks failed (257)`. The same low-memory condition caused LVGL heap fragmentation stalls, making double-press detection unreliable.

**Result**: NimBLE uses ~25KB SRAM (vs ~65KB Bluedroid), freeing ~40KB.

**Symptoms fixed**:
- ✅ SD card write failures every 30s — eliminated
- ✅ Double-press detection restored (heap stalls gone)
- ✅ ~40KB SRAM headroom when BLE active (was ~2–7KB)

**API changes** in `beacon_proximity.cpp`:
- Single `#include <NimBLEDevice.h>` replaces three Bluedroid headers
- `BLEDevice::*` → `NimBLEDevice::*`
- `BLEAdvertisedDeviceCallbacks` → `NimBLEAdvertisedDeviceCallbacks`
- `onResult(BLEAdvertisedDevice)` → `onResult(NimBLEAdvertisedDevice*)`
- Scan completion: timer-based → `g_pScan->isScanning()` (more reliable)
- SRAM guard threshold lowered: 60KB → 30KB
- All business logic (EMA, zones, hysteresis, trend, sonar) unchanged

**Files**:
- `platformio.ini` — added `h2zero/NimBLE-Arduino@^1.4.0`
- `src/hardware/connectivity/beacon_proximity.cpp` — NimBLE API rewrite

**Build impact**: −8,184 bytes static RAM (RAM: 59.3% / 194,152 bytes)

---

**SRAM Optimization — System Logger Heap Allocation**

System logger `g_buffer` (8KB) was a static `.bss` allocation — always in SRAM, even when logging was disabled.

**Fix**:
- `g_buffer` converted from `static char[8192]` to heap-allocated `char*` in `init()`
- `init()` only called when `settings.logging_enabled == true`
- `close()` frees the buffer
- **Normal mode: 0 bytes used** by the logger (no buffer, no mutex, no SD overhead)
- DEV mode with logging on: 8KB allocated from heap on demand

**Files**:
- `src/utils/system_logger.cpp` — heap allocation/free
- `src/core/main.cpp` — init gated behind `settings.logging_enabled`

**Build impact**: −8,184 bytes static RAM (included in NimBLE total above)

---

**SRAM Optimization — NTP Sync Removal**

WiFi NTP was running a 10-second polling loop in `loop()` even when WiFi was fully disabled. RTC (PCF85063) and GPS time sync via `task_manager::queueGPSTimeSync` provide all needed accuracy.

**Fix**: NTP polling timer removed from `loop()`. `ntp_sync::init(false)` disables auto-sync at boot.

**Files**: `src/core/main.cpp`

---

**LVGL Full-Frame Buffers (480 lines)**

Upgraded LVGL draw buffers from 160 lines to 480 lines (full frame). Eliminates the visible top-to-bottom screen wipe artifact that appeared during transitions when LVGL software rotation is active (the rotation requires a temporary buffer that fits cleanly in the 64KB `LV_MEM_SIZE`).

**Details**:
- Full frame: 480 × 480 × 2 bytes × 2 buffers = **921 KB PSRAM**
- 1 flush per frame (was 3 at 160 lines)
- `BUFFER_LINES = 480` in `include/core/system_config.h`

**Files**: `include/core/system_config.h`

---

### Fixed

**DEV Mode No Longer Forces Logging On**

DEV mode was calling `settings_manager::saveLoggingEnabled(true)` during boot, overwriting the user's `logging: OFF` NVS setting on every restart. User had to re-disable logging after every reboot.

**Fix**: Removed auto-enable block. Logging state is fully user-controlled.

**Files**: `src/core/main.cpp`

---

**WiFi Boot Settings Not Applied to Scanner**

`scanner.cpp` had a hardcoded `wifi_enabled = true` that was never synced from NVS. WiFi scanning ran at boot even with WiFi disabled in Settings, producing spurious scan errors in serial output.

**Fix**: Added `scanner::setWiFiEnabled(settings.wifi_enabled)` at boot alongside `wifi_manager::setEnabled()`.

**Files**: `src/core/main.cpp`

---

**GPX Description Truncation**

Three internal `tmp[]` buffers in `gpx_loader.cpp` were 192–256 bytes — too small for geocaching long-form descriptions, causing mid-sentence cuts in the waypoint detail screen.

**Fix**:
- All three buffers increased to 512 bytes (matches the line read buffer size)
- Added "..." truncation at last sentence boundary when the 1024-byte description buffer fills

**Files**: `src/gpx/gpx_loader.cpp`

---

**Waypoint Detail Screen — HINT Section Unreachable**

Long descriptions pushed the HINT section below the reachable scroll area, making it inaccessible even by scrolling to the bottom.

**Fix**: HINT section placed before DESCRIPTION in the flex column layout.
- HINT shows a tap-to-reveal header, hidden text by default
- DESCRIPTION scrolls below
- User can always reach HINT at the top of the scroll view

**Files**: `src/ui/waypoint_screen.cpp`

---

### Added

**Beitian BH-880 GPS+Compass Module**

Replaced Quectel LC76G GPS module with Beitian BH-880 (B1301N GPS chip + QMC5883L compass). Drop-in replacement on same UART pins (GPIO43/44, 115200 baud).

**GPS improvements**:
- 10Hz update rate (was 1Hz)
- Multi-constellation: GPS, GLONASS, BDS, Galileo, IRNSS, SBAS, QZSS (120 channels)
- Sensitivity: −163 dBm tracking, −148 dBm cold start
- Cold start: 28s, hot start: 1s

**Critical power note**: Module requires 3.6–5.5V. The board 3.3V rail is insufficient for a GPS fix. Solution: power VCC directly from LiPo positive (3.7–4.2V). QMC5883L compass rated 2.16–3.6V — works fine on 3.3V.

**Files**:
- `include/hardware/sensors/gps_bh880.h` / `src/hardware/sensors/gps_bh880.cpp`

---

**QMC5883L Compass System**

Replaced IMU/Gyro heading fusion with the QMC5883L magnetometer built into the BH-880 module. Compass is the sole heading source — GPS heading fusion removed.

**Architecture**:
- System Task reads QMC5883L every ~1s → queues `COMPASS_UPDATE` → UI Task applies to `ui.current_heading`
- All waypoints, off-screen indicators, and north indicator rotate from `ui.current_heading`
- Reaction time: ~1s. Smooth for walking speed.

**Critical I2C constraint**: Compass cannot be read from the I2C Task. The CST820 touch driver calls `Wire.requestFrom()` directly, bypassing `i2c_mutex`. Reading compass from I2C Task causes immediate `Wire.cpp requestFrom Error -1`. Must use System Task (tolerates occasional errors).

**Reference**: `docs/compass_i2c_constraint.md`

**Files**:
- `include/hardware/sensors/compass_qmc5883l.h` / `src/hardware/sensors/compass_qmc5883l.cpp`
- `src/utils/task_manager.cpp` — System Task reads + `COMPASS_UPDATE` queue message

---

**WMM Magnetic Declination**

Auto-computes magnetic declination from GPS fix using a truncated WMM2020 spherical harmonic model (n=1..3, ±1° accuracy). Applied to every compass reading to produce true heading.

**Behavior**:
- Computed once per session at first valid GPS fix
- Persisted to NVS, reused on subsequent boots until a new fix updates it
- Sign convention: `true_heading += declination` (positive = East declination)
- Example: Los Angeles area ~12.25° East at (34.13°N, 118.15°W) in 2026.2

**Files**:
- `include/utils/wmm_declination.h` / `src/utils/wmm_declination.cpp`

---

**Build: NimBLE Library**

Added `h2zero/NimBLE-Arduino@^1.4.0` to `platformio.ini` lib_deps.

---

## [v0.14.0] - 2026-01-30

### Added

**Beacon Proximity System**

BLE-based item finder that turns the radar into a proximity detector. When zoomed to 50m, the device scans for a configured BLE beacon MAC address and provides real-time visual + audio feedback.

**Visual Arc Gauge**:
- Cyan arc drawn clockwise around the radar circle outer edge
- Fills from 0° (no signal, -90 dBm) to 355° (full circle, -45 dBm) based on EMA-smoothed RSSI
- 14px line width at 228px radius — clearly visible without obscuring radar content
- Minimum 10° arc when any signal detected (always visible when in range)

**Audio Sonar Beeping**:
- Buzzer pulses at 1800ms (far) → 900ms → 500ms → 200ms (< 1m) as signal strengthens
- Can be independently disabled in Settings > Sound > Beacon Sound toggle
- Non-blocking state machine — zero impact on UI responsiveness

**RSSI Processing**:
- EMA smoothing (α = 0.4) prevents visual/audio jitter from signal fluctuations
- Zone-based detection (OUT_OF_RANGE / FAR / MEDIUM / CLOSE) with ±3 dBm hysteresis
- Requires 2 consecutive readings to confirm zone change (prevents oscillation)
- Linear trend detection over last 10 samples: APPROACHING / DEPARTING / STABLE

**Integration**:
- **Zoom-gated**: Only activates at 50m zoom — stops automatically when zooming out
- **BLE scanning**: 1s scan every 2s (50% duty cycle), early-exit when target MAC found
- **15s timeout**: Beacon marked lost if not seen for 15 seconds

**Settings (NVS persistent)**:
- Target MAC address, measured power (dBm @ 1m), path loss exponent
- Separate toggle for sound vs visual
- Keys: `bcn_en`, `bcn_snd`, `bcn_mac`, `bcn_pwr`, `bcn_n`

**Build Impact**: ~3,500 bytes flash, ~2KB RAM

**Files**:
- `include/hardware/connectivity/beacon_proximity.h` - API and state structures
- `src/hardware/connectivity/beacon_proximity.cpp` - BLE scanning, EMA, zone logic
- `src/ui/navigation.cpp:390-420` - `drawBeaconProximityGauge()` arc drawing
- `src/utils/task_manager.cpp:79-94` - Zoom-gating activation logic
- `src/utils/diagnostics.cpp:1255-1403` - Serial diagnostic commands
- `src/ui/settings_screen.cpp:1241-1310` - Beacon Sound settings toggle
- `src/utils/settings_manager.cpp:656-705` - NVS persistence

**Serial Commands**: `beacon status`, `beacon scan`, `beacon test`, `beacon zone`, `beacon trend`, `beacon debug`, `beacon reset`, `beacon mac/power/n`

**Reference Documentation**: [`docs/beacon_proximity.md`](docs/beacon_proximity.md)

---

**Gyro Calibration System (Calib Tab)**

New dedicated Calibration tab in Settings for gyro-based heading fusion. Provides smooth heading tracking when GPS is unreliable (stationary or slow walking).

**Features:**
- **Calib Tab**: Always visible in Settings (between Sound and DEV)
- **Two UI States**: DISABLED / RUNNING (calibrated) or NEEDS CALIBRATION
- **50m Zoom Activation**: Gyro only runs at 50m zoom to save resources
- **Persistent Calibration**: Saved to NVS, survives reboots indefinitely
- **One-time Calibration**: Calibrate once, valid for months/years
- **UI Feedback**: Shows "CALIBRATING..." during calibration process

**User Workflow:**
1. Enable toggle in Settings > Calib
2. Press Calibrate button (keep device still for ~5 seconds)
3. Status changes to "RUNNING" with "Calibrated: YES (saved)"
4. Gyro auto-activates when zooming to 50m for precision navigation

**Key Files:**
- `src/ui/settings_screen.cpp:2181-2400` - Calib tab UI and status display
- `src/utils/task_manager.cpp:69-106` - Zoom-based gyro activation
- `src/navigation/imu_sampling.cpp` - 100Hz gyro sampling and calibration
- `src/utils/settings_manager.cpp:669-703` - NVS persistence

**Technical Details:**
- QMI8658 IMU at 100Hz sampling rate
- Calibration requires 500 samples (~5 seconds)
- Bias values stored in NVS (imu_cal, imu_bx, imu_by, imu_bz)
- Heap allocation for calibration buffer (avoids stack overflow)

**Build Impact**: +500 bytes flash (minimal)

---

### Fixed

**CRITICAL: UI_Task Freeze After Extended Runtime (Priority Alpha)**

Fixed a thread-safety bug that caused the UI_Task to freeze after 7+ hours of runtime. The freeze was caused by unsafe LVGL calls from button callbacks during standby enter/wake operations.

**Root Cause Analysis:**
- `enterStandby()` and `wakeFromStandby()` were called directly from button callbacks
- Button callbacks run during `button::update()` which is OUTSIDE the display_mutex
- Both functions made LVGL calls (lv_obj_create, lv_timer_create, etc.) without mutex protection
- `wakeFromStandby()` also called `lv_timer_handler()` directly - potentially recursive/concurrent
- Over time, this corrupted LVGL internal state, causing UI_Task to hang

**Solution:**
1. Added `ENTER_STANDBY` and `WAKE_STANDBY` to UIUpdateType enum
2. Button callbacks now queue standby operations instead of direct calls
3. Standby operations are processed inside display_mutex (thread-safe)
4. Removed dangerous `lv_timer_handler()` call from `wakeFromStandby()`

**Key Changes:**
- `include/utils/task_manager.h:96-97` - Added ENTER_STANDBY, WAKE_STANDBY enum values
- `src/core/device_manager.cpp:635-696` - Queue standby ops instead of direct calls
- `src/utils/task_manager.cpp:548-560` - Handle ENTER_STANDBY/WAKE_STANDBY in processUIUpdate()
- `src/utils/standby_manager.cpp:126` - Removed unsafe lv_timer_handler() call

**Symptoms Fixed:**
- UI_Task becomes UNRECOVERABLE after ~452 minutes (7.5 hours)
- Button presses stop working (zoom, settings, etc.)
- Position stops updating on radar display
- Recovery attempts (suspend/resume) fail

**Build Impact**: +352 bytes flash (minimal)

### Added

**Button Sound Diagnostic Feature (Priority 2.11 Phase 1)**

Implemented basic buzzer functionality for diagnosing UI freeze issues. When enabled, a short chirp plays on button press, helping determine if the button hardware works when the UI becomes unresponsive.

**New Files**:
- `include/hardware/buzzer.h` - Buzzer interface
- `src/hardware/buzzer.cpp` - Buzzer implementation using TCA9554 EXIO pin 7

**Settings Changes**:
- New "Sound" tab in Settings (between Display and DEV)
- "Button Sound" toggle (default: OFF)
- "Test Beep" button for testing buzzer

**Settings Storage**:
- New NVS key `btn_sound` for button sound preference
- New `button_sound_enabled` field in RadarSettings

**Key Changes**:
- `src/hardware/input/button.cpp:118-131` - Plays chirp on button press when enabled
- `src/core/device_manager.cpp:698-703` - Buzzer initialization after button init
- `src/core/device_manager.cpp:707` - Buzzer update for timing management
- `src/ui/settings_screen.cpp:1262-1370` - Sound tab implementation
- `include/ui/ui_manager.h:119` - Added settings_tab_sound

**Build Impact**: +1.5KB flash (minimal)

---

## [v0.13.0] - 2026-01-21

### Added

**5-Phase Stability Overhaul - Thread-Safe UI Architecture**

Complete system stability rewrite to fix crashes caused by thread-unsafe LVGL access from button callbacks. The system now uses queue-based UI updates, mutex protection, hardware watchdog, enhanced crash logging, and task health monitoring.

**Phase 1: Queue-Based UI Updates**
- Button callbacks now queue UI requests instead of direct LVGL calls
- New UIUpdateType enum values: `ZOOM_CHANGE`, `ZOOM_CHANGE_REVERSE`, `SETTINGS_SCREEN`
- `processUIUpdate()` handles all UI operations safely within UI Task context
- Eliminates race condition between button ISR and LVGL timer handler

**Implementation**:
```cpp
// BEFORE (unsafe - caused crashes):
ui.cycleZoom();
navigation::updateRadarDisplay();

// AFTER (safe - queued):
UIUpdate update;
update.type = UIUpdateType::ZOOM_CHANGE;
task_manager::queueUIUpdate(update);
```

**Phase 2: Mutex Protection**
- `display_mutex` wraps `lv_timer_handler()` and UI queue processing
- `ui_state_mutex` protects UIState field access
- Thread-safe accessor functions: `getCurrentZoomLevel()`, `cycleZoomForward()`, `cycleZoomBackward()`
- `withDisplayMutex()` helper for safe LVGL operations from any context

**Phase 3: ESP32 Task Watchdog (TWDT)**
- Hardware-level detection of hung tasks
- 30-second timeout with warning (no panic by default)
- All tasks subscribe and feed watchdog every loop iteration
- ESP-IDF version compatibility (4.x and 5.x API support)
- New files: `include/utils/watchdog.h`, `src/utils/watchdog.cpp`

**Phase 4: Enhanced Crash Logging**
- `CrashInfo` structure with RTC memory persistence (survives reboot)
- `captureState()` records heap, PSRAM, task loops, last operation
- `logBootReason()` logs ESP32 reset reason on every boot
- `SYSLOG_CHECKPOINT()` macro for strategic crash location tracking
- Boot reason detection: power-on, software reset, brownout, panic, etc.

**Phase 5: Task Health Monitoring**
- `TaskHealth` structure tracks per-task health metrics
- `last_loop_time_ms` recorded every task iteration
- 5-second unresponsive threshold triggers recovery attempt
- `attemptTaskRecovery()` suspends/resumes hung tasks (max 3 attempts)
- Recovery logging with task identification

**Build Impact**: +4,892 bytes flash (1,402,149 bytes total, 44.6%), +48 bytes RAM

**Files Modified**:
- `include/utils/task_manager.h` - UIUpdateType enum, TaskHealth struct
- `src/utils/task_manager.cpp` - Queue processing, mutex, watchdog, health monitoring
- `src/core/device_manager.cpp` - Button callback uses queue instead of direct LVGL
- `include/utils/watchdog.h` - NEW: ESP32 TWDT wrapper API
- `src/utils/watchdog.cpp` - NEW: TWDT implementation with version compatibility
- `include/utils/system_logger.h` - CrashInfo struct, checkpoint macro
- `src/utils/system_logger.cpp` - RTC crash state, boot reason logging
- `src/core/main.cpp` - Watchdog initialization, boot reason logging

**Stability Improvements**:
- ✅ Zero crashes from rapid button presses (tested 10+ presses in 2s)
- ✅ No race conditions between button and UI tasks
- ✅ Hardware watchdog catches hung tasks before system freezes
- ✅ Crash state captured for post-mortem debugging
- ✅ Automatic task recovery attempts before giving up

---

**DEV Mode Enhancements**

Improved developer experience when DEV mode is enabled in settings.

**Auto-Enable Logging**:
- System logging automatically enabled when DEV mode is active
- Triggered on boot if `dev_tab_visible = true`
- Triggered when `dev show` command is used
- Serial output: `[DEV] Developer mode active - logging enabled`

**Power Management Override**:
- All automatic power management DISABLED when DEV mode is on
- No automatic brightness changes
- No automatic GPS preset changes
- No automatic standby entry
- Gives developer full manual control for testing
- Serial output: `[POWER] DEV MODE ACTIVE - All automatic power management DISABLED`

**Build Impact**: +156 bytes flash

**Files Modified**:
- `src/utils/power_manager.cpp` - Skip automatic power management when DEV mode enabled
- `src/utils/diagnostics.cpp` - Auto-enable logging on `dev show` command
- `src/core/main.cpp` - Auto-enable logging at boot when DEV mode already on

---

**Daylight Mode - High Contrast Outdoor Display**

New display mode optimized for outdoor visibility with bright sunlight.

**Color Scheme**:
| Element | Normal Mode | Daylight Mode |
|---------|-------------|---------------|
| Background | Dark green (`#3A9949`) | Light green (`#E0FFE0`) |
| Grid | Black | Black |
| Waypoints | Yellow with glow | Dark navy blue (`#000080`) |
| Center triangle | Red | Dark red |
| Glow effect | Enabled (soft yellow) | Disabled (no glow) |

**Shadow Overlay Adjustment**:
- Normal mode: 30% opacity (was 50%)
- Daylight mode: 0% opacity (invisible)
- Depth effect preserved in normal mode, maximum visibility in daylight

**HUD Labels with Background Containers**:
All HUD labels now have rounded background boxes for improved legibility:
- **Battery**: Dark green background (`#00AA00`), white text, 8px rounded corners
- **GPS Status**: Dark green background (`#006600`), white text
- **DEV Indicator**: Dark orange background (`#CC5500`), white text
- Backgrounds darken further in daylight mode for maximum contrast

**Settings Integration**:
- Toggle: Settings → Display → Daylight Mode
- NVS persistence: Setting saved across reboots
- Real-time switching: No restart required
- Description: "Use bright background for outdoor visibility"

**User Experience**:
- **Before**: Screen nearly invisible in direct sunlight
- **After**: High contrast colors readable outdoors
- **Result**: Usable navigation in all lighting conditions

**Build Impact**: +860 bytes flash, +16 bytes RAM

**Files Modified**:
- `include/settings_manager.h:40` - Added `daylight_mode = false` to RadarSettings
- `src/utils/settings_manager.cpp` - NVS load/save for daylight mode
- `include/ui/ui_manager.h` - Added shadow_overlay and HUD background references
- `src/ui/ui_manager.cpp` - HUD label backgrounds, updateDaylightMode() function
- `src/ui/navigation.cpp` - ColorScheme struct, getColorScheme(), light green daylight bg
- `src/ui/settings_screen.cpp:1201-1243` - Daylight Mode toggle in Display tab

**Brightness Verification**:
- Confirmed PWM at maximum (255 on 8-bit scale)
- Hardware limitation: Backlight is already at 100%
- Daylight mode compensates with high-contrast colors instead

---

## [v0.12.0] - 2025-10-26

### Added

**Independent Zoom Level Display**
- Zoom level now displayed separately from GPS status (always visible)
- Position: Bottom-center, above GPS status label
- Format: `[5km]` (in brackets for clear distinction)
- Updates independently when user changes zoom (single/double click)
- Works even when GPS is searching or disconnected

**User Experience**:
- **Before**: Zoom level only shown with GPS fix: "GPS: Fixed (8 sats) [100m]"
- **After**: Always visible: `[100m]` above "GPS: Searching..." or "GPS: Fixed (8 sats)"
- **Result**: User always knows current zoom level, even without GPS

**Visual Layout** (bottom-center):
```
        [5km]           ← Zoom level (always visible)
  GPS: Searching...     ← GPS status (changes color)
```

**Build Impact**: +148 bytes flash (1,394,769 bytes total, 44.3%), +16 bytes RAM

**Files Modified**:
- `include/ui/ui_manager.h:108` - Added `zoom_label` to UIState
- `src/ui/ui_manager.cpp:204-210` - Created zoom label above GPS status
- `src/ui/navigation.cpp:553-577` - Update zoom label independently from GPS status

---

**5km Zoom Level - Intermediate Regional View**
- New zoom level between 10km and 1km: **5km radius** with 1.25km grid spacing (60px)
- Zoom sequence: 10km → 5km → 1km → 500m → 100m → 10m (6 levels total)
- Provides better granularity for regional navigation (between city-wide and neighborhood views)
- Grid spacing follows progressive pattern: **48px → 60px → 80px → 96px → 120px → 160px** (squares get LARGER as you zoom in, all distinct)
- **User Feedback**: Grid squares visually distinguish each zoom level - 5km has medium squares between 10km (smallest) and 1km (larger)

**Center-Aligned Grid System**
- **All zoom levels** now have horizontal and vertical lines passing through screen center
- Creates smooth, natural zoom transitions - center crosshair remains anchored
- Grid spacing calculated to divide evenly into 240px (half-screen radius)
- Mathematical precision: Each zoom has exact integer number of grids per half-screen

**Grid Configuration** (480px screen width, all divisible for center line):
- **10km**: 48px spacing (10 lines total) ✓ Center line at 240px
- **5km**: 60px spacing (8 lines total) ✓ Center line at 240px
- **1km**: 80px spacing (6 lines total) ✓ Center line at 240px
- **500m**: **96px** spacing (5 lines total) ✓ Center line at 240px - **Distinct from 1km**
- **100m**: 120px spacing (4 lines total) ✓ Center line at 240px
- **10m**: **160px** spacing (3 lines total) ✓ Center line at 240px - **Distinct from 100m**

**User Experience**:
- **Before**: Some zooms had center lines, others didn't (felt "off-center")
- **After**: Every zoom has perfect center crosshair (horizontal + vertical lines)
- **Result**: Buttery smooth zoom transitions, user position always visually centered

**Waypoint Filtering Rules (Confirmed)**:
- **On-screen**: Waypoints within zoom radius (5km) shown as yellow circles
- **Off-screen arrows**: Waypoints within 10× radius (5-50km) shown as orange triangles
- **Filtered out**: Waypoints beyond 50km not displayed at 5km zoom
- **Same rule applies** to all zoom levels: 10km shows 0-10km on-screen, 10-100km as arrows

**Off-Screen Indicator Enhancement**:
- **Base width doubled** horizontally for better visibility at screen edges
- Height unchanged (maintains pointing direction clarity)
- Triangle now wider and more prominent without obscuring direction

**User Experience**:
- **Single Click**: Zoom in (10km → 5km → 1km → 500m → 100m → 10m → loops back to 10km)
- **Double Click**: Zoom out (reverse order: 10m → 100m → 500m → 1km → 5km → 10km)
- **Result**: More intuitive zoom control, better regional navigation granularity, improved waypoint visibility

**Build Impact**: +148 bytes flash (1,394,769 bytes total, 44.3%), +16 bytes RAM

**Files Modified**:
- `include/ui/ui_manager.h:27-34` - Added ZOOM_5KM enum, renumbered subsequent levels
- `include/ui/ui_manager.h:80-87` - All 6 zoom levels recalculated for center-aligned grids
- `src/ui/navigation.cpp:557-563` - Added "5km" label in zoom display switch
- `src/ui/navigation.cpp:374-398` - Doubled base width of off-screen indicator triangles
- `src/core/device_manager.cpp:590-603` - Changed double-click from resetZoom() to cycleZoomReverse()

### Changed

**Button Zoom Controls - Bidirectional Zoom**
- **Double-click behavior changed**: Now zooms OUT instead of resetting to default (100m)
- Single click: Zoom IN (10km → 5km → 1km → 500m → 100m → 10m)
- Double click: Zoom OUT (10m → 100m → 500m → 1km → 5km → 10km)
- Removed "reset to default zoom" feature (no longer needed with bidirectional control)

**User Experience**:
- **Before**: Single click zooms in, double click resets to 100m (always interrupts workflow)
- **After**: Single click zooms in, double click zooms out (smooth bidirectional control)
- **Result**: Natural zoom control without workflow interruption

**Build Impact**: No size change (function call replacement only)

**Files Modified**:
- `src/core/device_manager.cpp:591` - Serial log: "zooming out" instead of "resetting zoom to default"
- `src/core/device_manager.cpp:600` - Call `ui.cycleZoomReverse()` instead of `ui.resetZoom()`

---

**Standby Mode - Low-Power Sleep Function**
- GPIO0 4-second press enters standby mode (display OFF, GPS ON, WiFi/AP OFF)
- Any GPIO0 press wakes from standby, returning to previous screen
- Power consumption reduced from ~520mA (active) to ~55mA (standby) = 89% reduction
- Battery life: 5.8 hours active → 54 hours standby (3000mAh battery)
- Standby screen shows: "STANDBY MODE", battery %, time, "Press button to wake"
- 3-second transition screen before display turns OFF
- Statistics tracking: total standby count, total time, last duration

**User Experience**:
- **Entering Standby**: Hold GPIO0 for 4 seconds (2s more after Settings opens)
- **Standby Screen**: Black screen with white text, shows for 3 seconds
- **Display OFF**: Backlight fades to 0%, GPS continues tracking in background
- **Waking**: Press GPIO0 once, display turns ON, returns to exact same screen (Settings or Radar)
- **Result**: Long-term field use without draining battery, GPS track never interrupted

**Technical Architecture - Overlay Approach**:
- **Problem Solved**: Original screen-switching approach caused LVGL object invalidation and NULL pointer crashes
- **Solution**: Full-screen overlay on top of current screen instead of loading new screen
- **Key Insight**: `lv_obj_create(current_screen)` creates child overlay, preserving parent screen objects
- **Wake Mechanism**: Simply delete overlay → underlying screen reappears (no navigation needed)
- **Advantage**: Wakes to exact same screen you left, no object corruption, simpler code

**Power Settings Applied in Standby**:
- Display backlight: 0% (PWM OFF)
- WiFi scanning: Disabled
- AP mode: Disabled
- GPS module: Remains ON (continuous tracking requirement)
- Task update rates: Unchanged (future optimization opportunity)

**Button State Machine Enhancement**:
- Added `EXTRA_LONG_PRESS` event type (4-second threshold)
- Dual-threshold detection: checks 4s first, then 2s (prevents Settings opening during standby entry)
- Button state includes `extra_long_press_triggered` flag

**Standby Manager Module** (`src/utils/standby_manager.cpp`, `include/utils/standby_manager.h`):
- `enterStandby()` - Creates overlay, saves state, starts 3s timer
- `wakeFromStandby()` - Restores power settings, removes overlay
- `isStandby()` - Query current state
- `getStats()` - Retrieve usage statistics
- `StandbyState` enum: ACTIVE, ENTERING, STANDBY, WAKING

**Implementation Details**:
```cpp
// Create overlay on current screen (not new screen!)
lv_obj_t* current_screen = lv_scr_act();
g_standby_screen = lv_obj_create(current_screen);  // Child of current screen
lv_obj_set_size(g_standby_screen, LV_HOR_RES, LV_VER_RES);
lv_obj_set_style_bg_color(g_standby_screen, lv_color_black(), 0);
lv_obj_move_foreground(g_standby_screen);  // Bring to front

// Wake: simply delete overlay
lv_obj_del(g_standby_screen);  // Current screen reappears
```

**Critical Bug Fixed During Development**:
- **Issue**: NULL pointer crash (LoadProhibited at 0x00000020) when waking from standby
- **Root Cause**: Original `lv_scr_load()` approach invalidated radar canvas objects
- **Solution**: Overlay approach eliminates screen switching entirely
- **Result**: Zero crashes, clean wake transition

**Build Impact**: +3,592 bytes flash (1,394,653 bytes total, 44.3%), +32 bytes RAM

**Files Added**:
- `include/utils/standby_manager.h` - Public API and types
- `src/utils/standby_manager.cpp` - Implementation (280 lines)

**Files Modified**:
- `include/hardware/input/button.h:14-20` - Added EXTRA_LONG_PRESS event
- `include/hardware/input/button.h:26` - Added extra_long_press_ms config
- `include/hardware/input/button.h:37` - Added extra_long_press_triggered flag
- `src/hardware/input/button.cpp:62-84` - Dual-threshold detection logic
- `src/core/device_manager.cpp:6` - Added standby_manager include
- `src/core/device_manager.cpp:412-420` - Added EXTRA_LONG_PRESS case, standby wake check
- `src/core/main.cpp:29` - Added standby_manager include
- `src/core/main.cpp:294-297` - Added standby manager initialization
- `src/ui/navigation.cpp:62-77` - Added radar canvas validation (defensive)
- `src/ui/navigation.cpp:527-545` - Enhanced updateRadarDisplay() validation

**Serial Commands**: None (future: `standby stats`, `standby enter`, `standby wake`)

**Reference Documentation**: [`docs/standby_mode.md`](docs/standby_mode.md) - Complete technical guide (to be created)

---

## [v0.11.0] - 2025-10-21

### Changed

**Display Rotation System (Enclosure Design Adaptation)**
- Software rotation to compensate for 90° CCW physical display rotation in enclosure
- LVGL software rotation configured BEFORE driver registration (critical for RGB panels)
- User sees UI upright despite physical display orientation change
- Touch input automatically transformed to match rotated display

**User Experience**:
- **Before**: UI would appear sideways if display physically rotated in enclosure
- **After**: Software 90° CW rotation compensates, UI appears upright to user
- **Result**: Seamless adaptation to enclosure design changes without hardware modifications

**Technical Details**:
- **Rotation Method**: LVGL 8.x `disp_drv.sw_rotate = 1` + `disp_drv.rotated = LV_DISP_ROT_90`
- **Configuration**: `system_config::display::ROTATION_DEGREES = 90` constant
- **Critical Timing**: Rotation must be set BEFORE `lv_disp_drv_register()` call
- **RGB Panel Limitation**: Post-registration `lv_disp_set_rotation()` only transforms touch, not graphics
- **Touch Transform**: LVGL automatically adjusts touch coordinates for rotated display
- **Frame Buffer**: No changes required - software rotation handles pixel remapping

**Implementation Approach**:
```cpp
// CORRECT: Set rotation properties before registration
disp_drv.sw_rotate = 1;
disp_drv.rotated = LV_DISP_ROT_90;
lv_disp_t* disp = lv_disp_drv_register(&disp_drv);

// WRONG: Post-registration only rotates touch input
lv_disp_t* disp = lv_disp_drv_register(&disp_drv);
lv_disp_set_rotation(disp, LV_DISP_ROT_90);  // Graphics stay unrotated!
```

**Build Impact**: No flash/RAM impact (compile-time constant, existing LVGL feature)

**Files Modified**:
- `include/core/system_config.h:36-37` - Added `ROTATION_DEGREES = 90` constant
- `src/core/device_manager.cpp:2` - Added `#include "system_config.h"`
- `src/core/device_manager.cpp:453-477` - Implemented pre-registration software rotation
- `CLAUDE.md:223-256` - Added Display Rotation documentation section

**Known Limitations**:
- Rotation is compile-time constant (requires rebuild to change)
- Only supports 90°/180°/270° rotations (LVGL limitation, not arbitrary angles)
- RGB panels require software rotation (hardware rotation not supported)

**Future Enhancements**:
- Runtime rotation selection via settings UI (if needed for different enclosure variants)
- NVS persistence of rotation preference

**Reference Documentation**:
- LVGL 8.x Display Rotation: https://docs.lvgl.io/8.3/porting/display.html#rotation
- ESP32 RGB Panel Characteristics: Requires pre-registration rotation configuration

### Improved

**Screen Tearing Reduction (Extra-Large LVGL Buffers)**
- Increased LVGL buffer size to 160 lines (maximum practical size)
- Minimizes "staircase" tearing artifacts during scrolling and screen transitions
- Reduces visible split/offset rendering where half of elements update before the other half

**User Experience**:
- **Before**: Visible diagonal "staircase" pattern during screen transitions (radar → settings)
- **Before**: Scrolling showed split buttons/elements (half moves first, other half follows)
- **After**: Significantly reduced tearing (3 flush operations instead of 10)
- **Result**: Much smoother display updates, though not completely tear-free

**Technical Details**:
- **Root Cause**: Asynchronous DMA transfers from PSRAM while display scans (no sync mechanism)
- **Attempted Solution**: Hardware bounce buffer (not supported in this ESP-IDF version)
- **Actual Solution**: Dramatically increased LVGL buffer size from 120 to 160 lines
- **Buffer Configuration**: 160 lines × 480 pixels × 2 bytes × 2 buffers = 295KB PSRAM
- **Flush Reduction**: 480÷160 = 3 flushes per frame (vs 4 with 120 lines, 10 with 50 lines)

**Why Larger Buffers Help**:
- Each flush operation creates a visible horizontal band during scrolling
- 10 bands (50-line buffers) were very noticeable
- 4 bands (120-line buffers) were less visible
- 3 bands (160-line buffers) are even less perceptible
- Partial refresh mode only updates changed areas for performance

**Limitations**:
- Tearing cannot be completely eliminated without hardware sync (VSYNC callback or bounce buffer)
- ESP-IDF version used doesn't support `bounce_buffer_size_px` feature
- 160 lines is practical maximum (larger buffers provide diminishing returns)

**Build Impact**: +73KB PSRAM usage (295KB total vs 221KB with 120 lines)

**Files Modified**:
- `include/core/system_config.h:34` - Changed `BUFFER_LINES` from 120 to 160
- `CHANGELOG.md:68-98` - Documented tearing reduction approach and limitations

---

**Smooth Scrolling in Settings UI (Display Buffer Optimization)**
- Dramatically increased LVGL buffer size from 50 to 120 lines
- Fixed visible horizontal band/block artifacts during Settings screen scrolling
- Reduced flush operations from 10 to 4 per frame (60% reduction)

**User Experience**:
- **Before**: Visible horizontal bands/blocks during scrolling (10 separate flush operations)
- **After**: Smooth, professional scrolling without tearing artifacts (only 4 flush operations)
- **Result**: Settings UI feels polished and responsive like commercial products

**Technical Details**:
- **Root Cause**: Small 50-line buffers required 10 flush operations per full screen update
- **Solution**: Increased buffer size to 120 lines (480×120 = 57,600 pixels per buffer)
- **Flush Reduction**: 480÷120 = 4 flushes per frame (vs 480÷50 = 10 flushes previously)
- **Rendering Behavior**: Fewer, larger chunks = less visible banding during scrolling
- **Refresh Mode**: Partial refresh (`full_refresh = 0`) for optimal performance

**Why Larger Buffers Work**:
- Each flush operation creates a visible horizontal band during motion
- 10 bands are very noticeable to the human eye during scrolling
- 4 bands are much less perceptible and create smoother visual experience
- Partial refresh mode still provides fast rendering by only updating changed areas

**Build Impact**: +129KB PSRAM usage (1.6% of 8MB available)

**Files Modified**:
- `include/core/system_config.h:34` - Changed `BUFFER_LINES = 50` to `BUFFER_LINES = 120`
- `src/core/device_manager.cpp:450-451` - Updated comment to reflect buffer optimization
- `CLAUDE.md:114-146` - Updated display optimization documentation

**Performance Impact**:
- CPU usage: No change (same partial refresh mode)
- Memory usage: +129KB PSRAM (221KB total for dual buffers)
- Flush operations: 60% reduction (4 vs 10 per frame)
- User perception: Significantly improved (smooth vs blocky scrolling)

---

## [v0.10.0] - 2025-10-20

### Added

**Heading-Up Navigation Mode (Priority 1 - Critical UX)**
- Radar rotates to match walking direction - user always moves "forward" on screen
- GPS heading extracted from NMEA RMC sentence (course and speed over ground)
- Automatic coordinate rotation system for waypoints and position indicators
- North indicator (red circle with white "N") shows true north relative to heading
- Stationary mode: maintains last heading for 10 seconds, then reverts to north-up
- Settings toggle: Switch between Heading-Up and North-Up modes (Settings > Display)
- NVS persistence: Navigation mode preference saved across reboots

**User Experience**:
- **Before**: North always at top, user must mentally rotate map when turning
- **After**: Walking direction always points up, map rotates automatically
- **Result**: Intuitive navigation matching Google Maps/Waze behavior

**Technical Details**:
- **GPS Heading**: Parsed from RMC fields 7-8 (speed knots, course degrees)
- **Heading Threshold**: 0.5 knots minimum speed for reliable heading
- **Rotation Algorithm**: `rotatePoint()` applies -heading rotation to all coordinates
- **North Indicator**: Calculated position at screen edge, rotates with heading
- **Coordinate Transform**: Applied in `latLonToScreen()` after Haversine calculation
- **Performance**: O(n) rotation per waypoint, <1ms for 50 waypoints @ 240MHz

**Build Impact**: +1,848 bytes flash (rotation system, north indicator, settings toggle)

**Files Modified**:
- `include/hardware/sensors/gps_lc76g.h:12-15` - Added course, speed, hasHeading to GPSData
- `src/hardware/sensors/gps_lc76g.cpp:37-90` - Parse RMC course/speed fields
- `include/ui/ui_manager.h:125-129` - Added heading state to UIState
- `src/ui/navigation.cpp:98-118` - Implemented rotatePoint() function
- `src/ui/navigation.cpp:158-161` - Apply rotation in latLonToScreen()
- `src/ui/navigation.cpp:248-286` - Added drawNorthIndicator() function
- `src/ui/navigation.cpp:528-541` - Heading update logic with stationary fallback
- `include/settings_manager.h:37` - Added heading_up_mode setting (default: true)
- `src/ui/settings_screen.cpp:1012-1051` - Navigation mode dropdown with NVS save
- `src/ui/ui_manager.cpp:54-59` - Load heading_up_mode from NVS on startup

**Memory Usage**:
- **Flash**: 1,342,041 bytes (42.7%) - was 1,340,193 bytes
- **RAM**: 100,772 bytes (30.8%) - unchanged

---

## [v0.9.0] - 2025-10-20

### Added

**WiFi/AP Auto-Disable in CRITICAL Power Mode (Priority 2.6 Phase 3)**
- Automatic WiFi scanning disable when battery ≤ 20%
- Automatic AP mode disable when battery ≤ 20%
- Prevents accidental battery drain from forgotten WiFi/AP connections
- User can manually re-enable WiFi/AP after automatic disable (override allowed)
- Serial logging for transparency: `[POWER] ✓ WiFi scanning disabled (Critical mode - auto power save)`
- Settings persistence: WiFi/AP state saved to NVS after auto-disable

**Technical Details**:
- Implementation: `src/utils/power_manager.cpp:262-279`
- Controlled by: `applyPowerMode(PowerMode::CRITICAL)` function
- Integration: Uses `scanner::setWiFiEnabled(false)` and `wifi_manager::setEnabled(false)`
- Settings fields: `settings.wifi_enabled`, `settings.wifi_ap_enabled`
- Power savings: ~80-120mA when WiFi disabled
- User override: Manual re-enable via Settings UI works immediately

**Build Impact**: +~200 bytes flash (WiFi/AP disable logic)

**Files Modified**:
- `src/utils/power_manager.cpp:1-7` - Added scanner.h include
- `src/utils/power_manager.cpp:262-279` - Implemented WiFi/AP auto-disable in CRITICAL mode

---

## [v0.8.0] - 2025-10-19

### Added

**Waypoint Glow Effect (Priority 3.8)**
- Static soft glow around all waypoint beacons for analog radar aesthetic
- Uses LVGL native shadow rendering (no sprite assets required)
- Configurable glow parameters via RadarConfig constants
- Soft yellow-white glow (color: 0xFFFF88) with 16% opacity
- 18-pixel glow radius with 2-pixel shadow spread
- Centered glow effect (no X/Y offset) for symmetric appearance
- Professional aviation-grade radar appearance

**Technical Details**:
- Implementation: LVGL shadow properties (shadow_width, shadow_color, shadow_opa, shadow_spread)
- Configuration: New RadarConfig constants in `ui_manager.h`
  - `WAYPOINT_GLOW_RADIUS = 18` (shadow width in pixels)
  - `WAYPOINT_GLOW_COLOR = 0xFFFF88` (soft yellow-white)
  - `WAYPOINT_GLOW_OPACITY = LV_OPA_40` (16% opacity)
  - `WAYPOINT_GLOW_SPREAD = 2` (shadow spread in pixels)
- Performance: <1ms additional render time for 8-10 on-screen waypoints
- Zero heap allocation: Pure LVGL styling

**Build Impact**: +~100 bytes flash (configuration constants + styling code)

**Files Modified**:
- `include/ui/ui_manager.h:68-72` - Added glow configuration constants
- `src/ui/navigation.cpp:319-325` - Applied glow effect to waypoint drawing descriptor

---

## [v0.7.1] - 2025-10-18

### Fixed

**GPIO0 Button Polling Fix (CRITICAL)**
- **Root Cause**: `button::update()` was NEVER called anywhere in the codebase
- **Symptom**: Button worked intermittently (timing-dependent)
- **Impact**: Settings menu inaccessible, zoom controls unreliable
- **Solution**: Added button polling to UI Task (`task_manager.cpp:90-92`)
- **Result**: Button now works 100% of the time
- **Build Impact**: +14,728 bytes flash

**Technical Details**:
- Button initialization was completing successfully
- Hardware interrupt fired but state machine never processed it
- Without polling loop, button was effectively non-functional
- Serial monitor presence changed timing (USB CDC affects interrupt latency)
- Fix: Poll button every 5ms in UI Task (highest priority, Core 1)

**Files Modified**:
- `src/utils/task_manager.cpp` - Added `device_manager::updateButton()` call

### Added

**Crash Logging System (ESP32 Core Dump)**
- ESP32 panic handler with 256KB flash partition
- Three new serial commands:
  - `crash dump` - View last crash information (PC address, crashed task, core dump version)
  - `crash info` - Show system capabilities and usage instructions
  - `crash clear` - Clear crash data (note: auto-overwrites on next panic)
- Automatic panic capture to flash (survives reboots)
- Program Counter (PC) tracking for crash location identification
- Crashed task identification (UI/I2C/Network/System)
- Core dump version and firmware SHA256 tracking
- Comprehensive troubleshooting workflow documentation

**System Capabilities**:
- ✅ Automatic panic capture to flash partition
- ✅ Survives reboot (persistent storage)
- ✅ Program counter (crash location)
- ✅ Crashed task identification
- ✅ Accessible via serial commands
- ⚠️ Limitations: PC requires firmware.elf for symbol lookup, single crash storage

**Configuration**:
- Enabled `CORE_DEBUG_LEVEL=3` in `platformio.ini`
- Uses existing 256KB coredump partition from `partitions.csv`

**Documentation**:
- Added "Crash Investigation Workflow" to `docs/troubleshooting.md`
- Pattern recognition guide (single crash vs reproducible bug)
- Common crash patterns (GPIO, battery, WiFi-related)
- Preventive monitoring checklist
- Advanced debugging with addr2line tool

**Build Impact**: +7,816 bytes flash

**Files Modified**:
- `platformio.ini` - Enabled core dump debug level
- `src/utils/diagnostics.cpp` - Added crash dump commands (~110 lines)
- `docs/troubleshooting.md` - Added crash investigation guide (~150 lines)

**WiFi/AP Mode Mutual Exclusion**
- Implemented user-requested behavior: WiFi and AP modes are now mutually exclusive
- Enabling WiFi → Automatically disables AP mode (stops GPX server, disconnects AP)
- Enabling AP → Automatically disables WiFi (disconnects from network)
- Both can be OFF for battery savings
- Clear serial logging with ⚠️ warnings for mode changes
- All state changes saved to NVS (persistent across reboots)
- UI toggles update automatically without user intervention

**User Experience**:
- Before: Both WiFi and AP could run simultaneously (confusing)
- After: Only one mode active at a time (clear, predictable)
- User controls: Enable desired mode, system handles coordination
- Serial feedback: Clear warnings when automatic changes occur

**Build Impact**: +1,100 bytes flash

**Files Modified**:
- `src/ui/settings_screen.cpp` - WiFi/AP coordination logic (~50 lines)

**Total Build Impact for v0.7.1**: +23,644 bytes flash (+1.8%), RAM unchanged

---

## [v0.7.0] - 2025-10-18

### Added

**GPX Waypoint Enhancements (Priority 2.8 - Quick Wins Phase 1)**
- Refresh Waypoints button in GPS settings tab
  - Location: After Factory Reset button
  - Action: Calls `gpx_loader::refreshGPXFiles()` to reload from SD
  - Button: "🔄 Refresh Waypoints" (blue, 200x40px)
  - Feedback: Serial log "Loaded X waypoints from SD card"
  - Updates radar display and waypoint count label
  - Implementation: `src/ui/settings_screen.cpp:1612-1632`

- Waypoint Count Indicator in GPS settings tab
  - Display: "Waypoints: 15/50" (current/max)
  - Color coding:
    - Green (0x00FF00): 0-30 waypoints
    - Yellow (0xFFFF00): 31-45 waypoints
    - Red (0xFF4444): 46-50 waypoints (approaching limit)
  - Dynamic updates via `updateWaypointCountLabel()` function
  - Implementation: `src/ui/settings_screen.cpp:1585-1610`

- Build impact: +752 bytes flash, no RAM change

### Improved

**Settings UI/UX Improvements (Priority 2.13)**
- Added 100px bottom padding to all settings tabs
  - GPS tab (line 628)
  - WiFi tab (line 774)
  - Display tab (line 844)
- Benefits:
  - Bottom elements can scroll to middle of screen
  - Improved touch accuracy for bottom buttons
  - Better readability of bottom text elements
  - Professional app-like scrolling experience
- Build impact: +16 bytes flash

**Waypoint Filtering System Documentation**
- Complete technical documentation in `docs/waypoint_filtering.md`
- Dual-strategy filtering system:
  - Distance-based filtering (10× zoom radius multiplier)
  - Sector-based clustering (maximum 8 off-screen indicators)
- Performance characteristics documented
- Algorithm references added to CLAUDE.md

---

## [v0.6.0] - 2025-10-17

### Added

**Loading Screen with LVGL Spinners (Priority 2.7)**
- Professional boot sequence with animated spinner
- Implementation details:
  - 75px spinner with ease-in-out animation
  - 2-second rotation period for smooth motion
  - Title: "GPS RADAR SYSTEM" (Iosevka 20pt)
  - Status: "Initializing..." (Iosevka 16pt)
  - Dark grey background (#262626)
  - 5-second display duration
  - Automatic transition to radar screen
- Benefits:
  - Professional startup experience
  - Clear system initialization indicator
  - Reduces user confusion during GPS lock
  - Smooth transition animations

**Key Files**:
- `src/ui/ui_manager.cpp` - Loading screen creation
- `src/core/main.cpp` - Boot sequence integration
- `include/core/system_config.h` - Configuration constants

**Actual Time**: 2 hours (as estimated)

---

## [v0.5.0] - 2025-10-15

### Added

**Battery Percentage Display on Radar Screen (Priority 1.6)**
- Always-visible battery percentage (top-right corner)
- Color-coded status indicators:
  - Green (0x00FF00): >70% battery
  - Yellow (0xFFFF00): 50-70% battery
  - Red (0xFF0000): <50% battery
- Auto-updates every 5 seconds via System Task
- Simple percentage-only format: "69%"
- Integrated with existing battery monitoring system

**Critical Implementation Details**:
- Position: `-150px from right, +20px from top`
  - Circular clipping requires aggressive inset
  - Initial `-50px` caused text cutoff and system crashes
- Short text format prevents cutoff on circular boundary
- Z-order: Created before shadow overlay for visibility
- Updates integrated into System Task (`task_manager.cpp`)

**Display Location**:
```
┌─────────────────────────────────┐
│                            69%  │ ← Top-right (-150px, +20px)
│                                 │
│         RADAR DISPLAY           │
│                                 │
│                                 │
│    GPS: Fixed (6 sats)          │ ← Bottom-center
└─────────────────────────────────┘
```

**Battery Monitoring vs Display**:
Two separate but connected systems:
1. **Monitoring System** (`battery.cpp`)
   - Collects voltage samples via GPIO4 ADC
   - Performs trend analysis (charging/discharging/stable)
   - Provides serial diagnostics
   - Controlled via `battery monitor on|off` command

2. **Display System** (`ui.battery_label`)
   - Visual percentage indicator on radar
   - Always visible and updating
   - Independent of serial monitoring setting

**Serial Commands**:
```
battery status         # Show voltage, percentage, state
battery monitor on     # Enable periodic serial logging (every 60s)
battery monitor off    # Disable periodic logging (UI still works)
battery voltage        # Show current voltage reading
battery history        # Show voltage trend history
```

**Key Files**:
- `include/ui/ui_manager.h:99` - Battery label field
- `src/ui/ui_manager.cpp:129-135` - Label creation with safe positioning
- `src/utils/task_manager.cpp:695-714` - Update logic with color coding
- `src/hardware/sensors/battery.cpp` - Battery monitoring system

**What We Skipped** (as requested):
- Charging icon/animation on display (use board CHG LED instead)
- Voltage display on screen (available via serial only)
- Complex battery state transitions on UI (serial only)

**Reference Documentation**:
- `docs/battery_monitoring.md` - Complete battery system guide
- `docs/battery_display_summary.md` - Implementation history

---

## [v0.4.0] - 2025-10-12

### Added

**GPS Settings UX Simplification (Priority 2.5)**

**Background**:
User testing identified two critical UX issues:
1. GNSS Systems Checkboxes (5 checkboxes) overwhelming for average users
2. Update Rate Manual Selection confusing and error-prone

**Solution: Smart Presets System**

**GNSS Systems → Preset Dropdown**:
- Replaced 5 checkboxes with intelligent preset system
- 5 options: Battery Saver, Balanced (default), Best Accuracy, Maximum, Custom...
- "Custom..." option opens modal with 5 checkboxes for advanced users
- Default: "Balanced" (GPS + GLONASS, ~55 satellites, global coverage)

**GNSS Preset Mappings**:

| Preset | Systems Enabled | Bitmask | Satellites | Use Case |
|--------|----------------|---------|------------|----------|
| Battery Saver | GPS only | `0x01` | ~31 | Longest battery life, acceptable accuracy |
| Balanced (Default) | GPS + GLONASS | `0x03` | ~55 | Best balance of accuracy/battery |
| Best Accuracy | GPS + GLONASS + Galileo | `0x07` | ~85 | High accuracy for navigation |
| Maximum | GPS + GLONASS + Galileo + BeiDou | `0x0F` | ~120 | Maximum accuracy, faster fix |
| Custom | User-defined | Variable | Variable | Advanced users only |

**Update Rate → Auto-Calculated**:
- Removed manual dropdown entirely
- Auto-calculate rate based on positioning mode:
  - Pedestrian → 1 Hz (1000 ms) - walking speed
  - Automotive → 5 Hz (200 ms) - driving speeds
  - Fitness → 2 Hz (500 ms) - running/cycling
  - Aviation → 10 Hz (100 ms) - high-speed flight
- Display: "Update Rate: 5 Hz (auto)" (read-only indicator)

**Implementation Complete**:
- ✅ Backend helper functions with comprehensive logging
- ✅ Help modal system (4 help icons)
- ✅ Phase 1-7 all completed and tested
- ✅ Comprehensive documentation (`docs/gps_settings_simplification.md`)

**Serial Logging**:
All GPS settings changes logged with consistent tags:
```
[GNSS_PRESET]   # GNSS preset selection and bitmask mapping
[GNSS_CONFIG]   # GNSS configuration breakdown
[AUTO_RATE]     # Auto update rate calculation
[GPS_SETTINGS]  # General GPS settings changes
[SETTINGS]      # NVS save/load operations
```

**Success Criteria Achieved**:
- ✅ Average users can configure GPS without technical knowledge
- ✅ Preset names are self-explanatory
- ✅ Update rate is automatically optimized
- ✅ Advanced users can still access full control via "Custom..."
- ✅ All settings persist across reboots

**External Zoom Button (Priority 1.1)**
- GPIO0 button used for zoom cycling
- Hardware button successfully implemented
- Touch screen freed for waypoint interaction
- Maintains 5-level zoom cycle behavior

**Settings Menu Trigger (Priority 1.2)**
- GPIO0 long-press detection (2-3 seconds)
- Enter/exit settings menu working
- Non-blocking button detection implemented
- Settings accessible via long-press

**NVS Storage System (Priority 2.1)**
- Non-Volatile Storage (NVS) for persistent settings
- User preferences stored across reboots
- Safe write operations with error handling
- Default values on first boot implemented

**Settings Menu UI (Priority 2.2)**
- Full-screen settings interface with tabbed navigation
- Touch-based navigation working
- Visual feedback for all controls
- Settings automatically saved (no explicit Save button)
- Red X button returns to radar screen

**Settings Categories**:
1. Display Settings (zoom, grid, brightness)
2. GPS Settings (update interval, logging, GNSS systems, positioning mode)
3. Waypoint Settings (persistent storage, max waypoints)
4. Advanced Settings (show coordinates, heading, speed)

---

## [v0.3.0] - 2025-10-12

### Added

**WiFi Web Portal for GPX Upload (Priority 3.1)**

**Architecture**: Station Mode + Web Portal
- User connects radar to home/office WiFi
- Web portal accessible at `http://radar.local` or device IP address
- No mode switching needed - works on existing WiFi network

**Current Implementation**:
- ✅ WiFi Manager - Full STA mode with credential storage
- ✅ GPX Server - Complete web server with upload UI (`gpx_server.cpp`)
- ✅ Beautiful drag-and-drop interface (HTML/CSS/JS embedded)
- ✅ RESTful API - Upload, list, delete endpoints
- ✅ mDNS support - `http://radar.local` automatic discovery
- ✅ Integration - GPX server integrated in main.cpp loop (auto-starts when WiFi connects)
- ✅ Auto-loading - GPX files auto-loaded from SD card on boot (`gpx_loader.cpp`)
- ✅ Web Portal UI - URL displayed in WiFi settings tab

**Known Issues**:
- ⚠️ Web portal only accessible in AP mode
- ⚠️ mDNS not working in STA mode
- ✅ Workaround: Use IP address displayed in WiFi settings tab

**Web Portal Features**:
- 📂 Drag-and-drop GPX file upload
- 📋 View uploaded GPX files with delete option
- 🎨 Beautiful responsive UI (purple gradient design)
- 🔄 Real-time upload progress and status messages
- 🌐 mDNS discovery - `http://radar.local`
- 📍 Auto-creates `/gpx/` folder on SD card

**Web Portal Endpoints**:
```
GET  /              # Upload interface (HTML page)
POST /upload        # File upload handler
GET  /list          # List GPX files (JSON)
DELETE /delete/:filename  # Delete GPX file
```

**User Workflow**:
1. Long-press GPIO0 → Settings screen
2. Connect to WiFi network (via WiFi UI)
3. Settings displays: "Web Portal: http://192.168.1.100"
4. Open browser on phone/laptop → Drag-and-drop GPX files
5. Waypoints automatically appear on radar

---

## [v0.2.0] - 2025-10-07

### Added

**Multi-Level Zoom System (Priority 1.3)**
- 5 zoom levels with progressive grid sizing
- Touch-to-zoom interface (tap canvas to cycle zoom)
- Dynamic meters-per-pixel calculation
- Grid spacing adapts to zoom level (48px → 140px squares)
- Zoom level displayed in GPS status text

**Zoom Levels**:

| Level | Radius | Grid Spacing | Grid Size (pixels) | Use Case |
|-------|--------|--------------|-------------------|----------|
| 10km | 10000m | 2000m | ~48px | Long-range navigation |
| 1km | 1000m | 300m | ~72px | Local area |
| 500m | 500m | 200m | ~96px | Neighborhood |
| 100m (default) | 100m | 50m | ~120px | Street level |
| 10m | 10m | 5.83m | ~140px | Precision mode |

**User Interaction**:
- Tap radar canvas to cycle zoom: 10km → 1km → 500m → 100m → 10m → (loop)
- Current zoom level shown in status text: `GPS: Fixed (15 sats) [100m]`

**Key Files**:
- `include/ui/ui_manager.h` - ZoomLevel enum, ZoomConfig struct
- `src/ui/ui_manager.cpp` - Touch-to-zoom event handler
- `src/ui/navigation.cpp` - Zoom-aware coordinate conversion

**Edge-Aligned Grid System (Priority 1.4)**
- Perfect edge alignment at all zoom levels
- Grid lines always reach screen edges (0 and 479 pixels)
- No visual gaps or offsets
- Dynamic grid spacing based on zoom level

**Algorithm**:
```cpp
// Draw vertical lines from x=0 with grid_spacing_pixels intervals
for (int x = 0; x < screen_size; x += grid_spacing_pixels) {
    draw_line(x, 0, x, screen_size-1);
}
// Always draw right edge line at x=479
if ((screen_size - 1) % grid_spacing_pixels != 0) {
    draw_line(screen_size-1, 0, screen_size-1, screen_size-1);
}
```

**Key Files**:
- `src/ui/navigation.cpp:drawRadarGrid()` - Grid drawing implementation

**Visual Polish and UI Refinements (Priority 1.5)**
- ✅ Fixed triangle color (black → red #D43701)
- ✅ Geometric centering of center triangle (using centroid offset)
- ✅ Changed waypoint markers to circles (25x25px yellow)
- ✅ Removed "+ Waypoint" button (clean full-screen radar)
- ✅ Repositioned GPS status text higher (y=-40 instead of y=-10)
- ✅ Reduced GPS serial logging spam (every 10 seconds instead of every second)
- ✅ Removed "[RADAR] Update display" debug spam
- ✅ Fixed canvas positioning (explicit 0,0 with no padding)

**GPS Status Label**:
- Position: `ALIGN_BOTTOM_MID, y_offset=-40`
- Font: `lv_font_montserrat_14`
- Colors:
  - Searching: Yellow (#FFFF00)
  - Fixed: Green (#00FF00)
- Format: `"GPS: Fixed (15 sats) [100m]"`

**GPS Integration (Priority 1.2)**
- LC76G GPS module integration (UART on GPIO43/44, 115200 baud)
- NMEA sentence parsing (GGA, RMC)
- Real-time position accuracy: ±1-2 meters (15 satellites, HDOP=1.0)
- Visual GPS status indicator:
  - Yellow text: "GPS: Searching..." (no fix)
  - Green text: "GPS: Fixed (X sats)" (locked)
- Automatic center reference update (user position becomes center)

**Performance**:
- Position update rate: 1Hz (every second)
- Serial logging: Every 10 seconds (reduced spam)
- Typical accuracy: ±0.78m latitude, ±0.46m longitude

**GPS Data Structure**:
```cpp
struct GPSData {
    double lat, lon;      // Decimal degrees
    float alt;            // Altitude in meters
    float hdop;           // Horizontal dilution of precision
    int sats;             // Number of satellites
    bool valid;           // Fix status
};
```

**Key Files**:
- `src/gps/gps_lc76g.cpp` - GPS driver and NMEA parsing
- `src/utils/task_manager.cpp` - GPS task and serial logging
- `include/device_manager.h` - GPS data structures

---

## [v0.1.0] - 2025-10-07

### Initial Release

**Core Radar Display System (Priority 1.0)**
- Full-screen circular 480x480 radar display (green background)
- Edge-aligned black grid system (2px lines)
- Red equilateral triangle in center (44x44px, geometrically centered)
- Yellow circular waypoint beacons (25x25px)
- User-centered navigation (center triangle represents user position)
- Waypoints move relative to user as GPS position updates

**Key Files**:
- `src/ui/ui_manager.cpp` - Radar screen creation and canvas setup
- `src/ui/navigation.cpp` - Drawing functions and coordinate conversion
- `include/ui/ui_manager.h` - Radar configuration constants

**Technical Implementation**:
```cpp
// Radar visual elements
CENTER_TRIANGLE_SIDE = 44px     // Equilateral triangle sides
CENTER_TRIANGLE_HEIGHT = 38px   // Triangle height
WAYPOINT_SIZE = 25px            // Yellow beacon circles
GRID_LINE_WIDTH = 2px           // Black grid lines
```

**Radar Display Features**:
- 480×480 pixel circular display
- Green background (#00AA00)
- Black grid overlay (2px lines)
- Red center triangle (user position)
- Yellow waypoint markers
- Real-time GPS coordinate tracking
- Serial debug output (115200 baud)

---

## Development Notes

### Documentation Updates
- Complete battery monitoring guide in `docs/battery_monitoring.md`
- Battery display implementation summary in `docs/battery_display_summary.md`
- GPS settings simplification guide in `docs/gps_settings_simplification.md`
- Waypoint filtering technical deep-dive in `docs/waypoint_filtering.md`
- Custom fonts documentation in `docs/custom_fonts.md`
- WiFi implementation guide in `docs/wifi_implementation_guide.md`

### Build System
- PlatformIO project with ESP32-S3 support
- 16MB Flash, 8MB PSRAM
- Custom partition table for app and filesystem
- LVGL 8.3.11 graphics library
- Arduino framework

### Hardware Requirements
- Waveshare ESP32-S3-Touch-LCD-2.1 board
- Beitian BH-880 GPS + Compass module (GPIO 43/44 UART, GPIO 15/7 I2C)
- 3.7V LiPo battery
- MT3608 boost converter (3.3V → 5V for GPS module)

---

**Project Repository**: https://github.com/alvroga/db-radar
**License**: CC BY-NC-SA 4.0 — Alvaro Robles

**Last Updated**: 2026-05-09
