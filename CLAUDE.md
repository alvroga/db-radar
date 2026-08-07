# CLAUDE.md - Waveshare ESP32-S3-Touch-LCD-2.1 Template

**Technical Documentation** for developers and AI assistants working with this template.

This document provides comprehensive technical guidance for the Waveshare ESP32-S3-Touch-LCD-2.1 development board template.

## Project Overview

This is a **production-ready PlatformIO template** for ESP32-S3 touch LCD development. It provides a complete hardware abstraction layer, modular architecture, and proven stability for the Waveshare ESP32-S3-Touch-LCD-2.1 board.

**Repository**: https://github.com/alvroga/db-radar
**License**: MIT

### **Hardware Platform**
- **Board**: Waveshare ESP32-S3-Touch-LCD-2.1 (16MB Flash, 8MB PSRAM)
- **MCU**: ESP32-S3 @ 240MHz with PSRAM support
- **Display**: 2.1" 480×480 IPS LCD (ST7701 controller)
- **Touch**: Capacitive touch (CST820 controller)
- **Connectivity**: WiFi, Bluetooth, USB-C (native USB)

## Key Configuration

### **Configuration System**

All magic numbers/timing/pins consolidate into `include/core/system_config.h`, organized by namespace
(`display`, `pins`, `communication`, `timing`, `ui`, `backlight`, `memory`, `features`), with runtime
inspection via serial (`config show|display|timing|pins`). Hardware variant selection (this board vs.
custom 320×240/240×320) is a single `#define` swap.

**Full guide**: [`docs/configuration.md`](docs/configuration.md).

### **✅ PROFESSIONAL TASK MANAGEMENT SYSTEM** (Priority 3.7 Complete)

The project now features a **comprehensive FreeRTOS multitasking architecture** that eliminates the I2C-based freezes and significantly improves system responsiveness. This addresses the core issue of 100ms UI freezes caused by blocking I2C operations.

**Task Architecture** (`src/utils/task_manager.cpp` and `include/utils/task_manager.h`):
- **UI Task** (Core 1, Priority 5) - LVGL processing and touch input for maximum responsiveness
- **I2C Task** (Core 0, Priority 2) - All device communication via queued requests with callbacks
- **Network Task** (Core 0, Priority 1) - WiFi/BLE scanning operations (can be disabled)
- **System Task** (Core 0, Priority 1) - Memory monitoring, diagnostics, and status updates

**Key Features:**
- **Queue-based I2C Communication**: All RTC and EXIO operations go through request queues
- **Cross-task UI Updates**: Safe status label updates via UI update queue
- **Task Health Monitoring**: Real-time statistics and health checks for all tasks
- **Robust Error Handling**: Comprehensive retry logic and failure reporting
- **Memory Safety**: Conservative stack sizing (16KB UI, 8KB I2C, 12KB Network, 8KB System)

```cpp
// Task configuration with proven safe parameters
struct TaskConfig {
    static constexpr size_t UI_STACK_SIZE = 16384;     // UI + LVGL processing
    static constexpr size_t I2C_STACK_SIZE = 8192;     // I2C operations
    static constexpr UBaseType_t UI_PRIORITY = 5;      // Above FreeRTOS timer svc (1), below WiFi driver (22-23)
    static constexpr BaseType_t UI_CORE = 1;           // Core 1 for UI
    static constexpr BaseType_t OTHER_CORE = 0;        // Core 0 for everything else
};
```

**`UI_PRIORITY` is a bracket, not a tuned value** — don't "tidy" it back down. Higher number = higher
priority in FreeRTOS (opposite of Unix `nice`), and priority only arbitrates *within a core*. The UI
Task is alone on Core 1; I2C/Network/System are all on Core 0, so their numbers never compete with it.
The only constraint is the one the comment states: stay above `loopTask` and the FreeRTOS timer
service (both priority 1, `CONFIG_FREERTOS_TIMER_TASK_PRIORITY=1`) and well below the WiFi/BT stack
(~22-23) and IPC tasks (24), which must outrank application code. Nothing in the project occupies 3
or 4, so any value in that gap schedules identically — this was 3 for a long time with no behavioural
difference, and the docs said 3 well after the code said 5.

**Task Management Commands** (via serial):
- `task status` - Show real-time task statistics and health
- `task enable ui|i2c|network|system on|off` - Enable/disable specific tasks
- Task statistics include loop counts, runtime, stack usage, and health status

All I2C-device access (RTC, EXIO, etc.) goes through queued requests to the I2C Task rather than
direct calls from other tasks — this is what eliminated the original bus-contention-driven UI freezes
this architecture replaced. See [`docs/i2c.md`](docs/i2c.md) for the current queue/device model.

### **PlatformIO Settings**

**The build is ESP-IDF, not Arduino.** The env is `cc-radar`; `cc-moat-port` and the Arduino
framework are pre-migration history. Behaviour-affecting settings live in `sdkconfig.defaults`, not
only here — and editing that file alone does **not** change the build (see the `sdkconfig` note in
the Render Pipeline section).

```ini
[env:cc-radar]
platform = espressif32
board = esp32-s3-devkitc-1
framework = espidf
board_upload.flash_size = 16MB
board_build.partitions = partitions/partitions_ota.csv
board_build.filesystem = fatfs
board_build.flash_mode = dio        ; ROM-loader mode only — runs QIO at runtime, see the .ini comment
board_build.f_flash = 80000000L
```

### **Memory Layout**
- **Custom Partitions**: `partitions/partitions_ota.csv` — 2 × 4MB OTA app slots + ~7.69MB FFat,
  grown from the original 2MB/11.7MB split 2026-08-06 (that split was an unexamined Arduino IDE
  default, never chosen for this project; see [ADR-0024](docs/adr/0024-ota-partitions-grown-from-unused-ffat.md)).
  4MB was chosen over an initial same-day 3.5MB pass because OTA headroom needs a full USB reflash to
  grow again while FFat headroom is freeable anytime over the web portal — see the ADR's addendum.
  FFat is now mounted (`device_manager::initFFat()`, `esp_vfs_fat_spiflash_mount_rw_wl()` at `/ffat`,
  right after SD init in `initializeAll()`) and holds GPX waypoint files (`/ffat/gpx`) — moved off SD
  the same day GPX-to-FFat was decided, see ROADMAP.md's "GPX Storage: Move from SD to FFat" (Resolved)
  and CHANGELOG.md. **The one-time SD→FFat auto-migration that originally lived in
  `gpx_loader::init()` is gone (removed 2026-08-07)** — it copied `/sdcard/gpx` → `/ffat/gpx` whenever
  the FFat folder was empty, which couldn't distinguish "never migrated" from "user just deleted
  everything," so a deliberate delete-all silently resurrected the old files from SD on next boot. A
  one-time NVS-gated cleanup ran once to remove the stale SD copies, then was itself deleted so no
  automatic-delete logic sits in the firmware going forward — FFat is the sole source of truth for GPX
  files now, no migration path exists in either direction. Dev-only
  logging (`system_logger`, `field_log`, `tilt_bench`) stays on the physical SD card, and its web
  management page (`/logs`) is gated behind `dev_mode` (404 when off) — not specifically field-tested.
  (`partitions/partitions.csv` is the orphaned pre-OTA 3MB/10MB table — unused, see its header.)
- **PSRAM**: octal PSRAM, 64-byte alignment for DMA/framebuffer allocations
- **Flash**: 16MB, running **QIO** despite `flash_mode = dio` above

## Development Commands

### **Build and Upload**
```bash
# Build the project
pio run

# Upload to device
pio run --target upload

# Build and upload in one command
pio run -t upload

# Clean build files
pio run -t clean
```

### **Monitoring and Debugging**
```bash
# Serial monitor (115200 baud)
pio device monitor

# Upload and immediately start monitoring
pio run -t upload && pio device monitor
```

### **Testing and Analysis**
```bash
# Run unit tests
pio test

# Static code analysis
pio check
```

## Hardware Integration

### **Critical Display Configuration**
The display requires **precise timing** to avoid jitter:

```cpp
// Stable timing baseline (post-jitter fix)
// 480×480 @ 10MHz PCLK
cfg.timings.pclk_hz = 10000000;
cfg.timings.hsync_pulse_width = 8;
cfg.timings.hsync_back_porch = 20;   // Critical: was 16, now 20
cfg.timings.hsync_front_porch = 20;  // Critical: was 16, now 20
cfg.timings.vsync_pulse_width = 4;
cfg.timings.vsync_back_porch = 8;
cfg.timings.vsync_front_porch = 10;  // Critical: was 8, now 10
// Polarities: all 0, pclk_active_neg=0, pclk_idle_high=0
```

### **Display Rotation**

**Physical Orientation**: 90° CCW (counter-clockwise) rotation due to enclosure design, compensated 90° CW.

**The default compensation path is a tiled transpose in the flush callback, not LVGL's `sw_rotate`** —
`disp_drv.sw_rotate = 0` in the default (TILED) mode, with `full_refresh = 1` required alongside it
(see load-bearing constraint #3 in the Render Pipeline section — this is not a free choice). Rotation
must still be configured **before** `lv_disp_drv_register()` for ESP32 RGB panels; post-registration
rotation only affects touch input, not graphics.

**Full detail** (why not `sw_rotate`, the SRAM scratch-tile mechanism, the `rot on|off|tiled` runtime
toggle for A/B measurement): [`docs/display.md`](docs/display.md) and
[ADR-0004](docs/adr/0004-tiled-transpose-display-rotation.md).

**Testing**: After any display/rotation change, verify touch alignment by tapping screen corners.

### **I2C Bus Architecture**

All I2C traffic goes through one unified manager (`i2c_manager.cpp`/`.h`, ESP-IDF 5.x
`driver/i2c_master.h`) — device handles, retry logic, per-device and cross-device failure stats, and
bus-recovery (`reinit()`, not `resetBus()` — see the doc for why). **Shared I2C Bus** (SDA=15, SCL=7)
@ 400kHz hosts **six** devices: Touch (CST820, 0x15), RTC (PCF85063, 0x51), IO Expander (TCA9554,
0x20), Compass (QMC5883L, 0x0D), and the IMU (QMI8658, two addresses 0x6A/0x6B) — the IMU is genuinely
present and in active use for tilt compensation; an older note elsewhere claiming it was removed for
bus contention is stale (GPS is UART, not I2C, so there was never a contention issue between the two).

```cpp
#include "i2c_manager.h"
i2c_manager::readByte(i2c_manager::RTC_DEVICE, 0x01, data);
i2c_manager::exio::set(i2c_manager::exio::BUZZER, false, exio_state);
```

**Full guide** (API, device table, stats/recovery, and the historical EXIO pin-mapping bug that once
stuck the buzzer on): [`docs/i2c.md`](docs/i2c.md).

### **GPIO Pin Assignments**
```cpp
// Display RGB Interface (16-bit)
#define DATA_PINS {5,45,48,47,21,14,13,12,11,10,9,46,3,8,18,17}
#define LCD_PCLK      41  // Pixel clock
#define LCD_DE        40  // Data enable
#define LCD_VSYNC     39  // Vertical sync
#define LCD_HSYNC     38  // Horizontal sync

// Control Interfaces
#define LCD_BL         6  // Backlight PWM
#define LCD_MOSI_PIN   1  // SPI command interface
#define LCD_CLK_PIN    2  // SPI clock
#define I2C_SDA       15  // Shared I2C bus
#define I2C_SCL        7  // Shared I2C bus

// External Peripherals
#define GPS_TX        43  // GPS module TX (optional config)
#define GPS_RX        44  // GPS module RX
#define SD_CLK         2  // SD card (shared with LCD_CLK)
#define SD_CMD         1  // SD card (shared with LCD_MOSI)
#define SD_D0         42  // SD card data
```

## Software Architecture

### **Module Organization**
**✅ MODULAR ARCHITECTURE** (Priority 1.1 Complete)

```
src/
├── main.cpp              # Clean entry point (80 lines, was 1308)
├── core modules/         # ✅ PRIORITY 1.1 COMPLETE
│   ├── device_manager.cpp    # Hardware initialization and LVGL callbacks
│   ├── ui_manager.cpp        # Screen creation and UI object management
│   ├── navigation.cpp        # Event handling, page logic, and timers
│   └── diagnostics.cpp       # Serial command interface and debug features
├── connectivity/
│   └── scanner.cpp           # WiFi/BLE scanning (clean implementation)
├── device drivers/
│   ├── lcd_st7701.cpp        # Display initialization
│   ├── cst820.cpp            # Touch controller
│   ├── backlight.cpp         # PWM backlight control
│   ├── rtc_pcf85063.cpp      # Real-time clock
│   ├── gps_bh880.cpp         # GPS UBX parsing (BH-880 module)
│   └── accel_qmi8658.cpp     # QMI8658 accelerometer, self-contained (own register constants, routes through i2c_manager)
└── i2c infrastructure/       # ✅ PRIORITY 1.2 COMPLETE
    ├── i2c_manager.cpp       # Unified I2C operations with retry logic
    └── I2C_Driver.cpp        # Legacy compatibility layer
```

### **LVGL Integration**
- **Version**: 8.3.11
- **Configuration**: `LV_CONF_INCLUDE_SIMPLE` mode
- **Memory**: Direct framebuffer access with dual buffers, `BUFFER_LINES = 480` (full frame)
- **Performance**: 10-line SRAM bounce buffer (18.75KB), 64-byte PSRAM alignment

### **Memory Management**

Real-time heap/PSRAM/LVGL/DMA monitoring, ultra-conservative fixed-size pools (768 bytes total — an
earlier 12KB+ design caused boot loops, see the doc for why smaller was the fix), automatic heap
corruption checks, and a full serial diagnostic interface (`memory stats|info|report|pools|cleanup|
integrity|leak|stress`).

**Full guide**: [`docs/memory_management.md`](docs/memory_management.md).

## Development Best Practices

### **✅ Hardware Initialization Order**
1. **I2C Bus** - Initialize first (needed by multiple devices)
2. **IO Expander** - Required for LCD_CS control
3. **Display** - ST7701 init via SPI commands, then RGB panel
4. **Touch** - CST820 after display is stable
5. **Peripherals** - RTC, GPS in any order
6. **Radio** - WiFi/BLE last (after display is rendering)

### **✅ Error Handling Patterns**
```cpp
// Always check I2C operations
if (!rtc::read(time_data) || !time_data.valid) {
    lv_label_set_text(label, "RTC: error");
    return;
}

// Throttle I2C operations to reduce bus errors
static uint32_t last_read = 0;
if (millis() - last_read >= 5000) {  // 5 second intervals
    // Perform I2C operation
    last_read = millis();
}
```

### **✅ Cross-Task Shared State**
Single-word globals (bool, pointer, uint32_t — atomic on Xtensa) may be shared across tasks
unsynchronized, read/written directly; struct-valued shared state needs a mutex, a critical section,
or a documented exception.

### **✅ Display Performance**
```cpp
// full_refresh tracks the rotation mode — 1 for TILED (the default), 0 otherwise.
// It is NOT a free choice: see load-bearing constraint #3 in the Render Pipeline section.
lv_disp_drv_t disp_drv;
disp_drv.full_refresh = 1;

// Enable bounce buffer
cfg.bounce_buffer_size_px = 10 * SCR_W; // ~10 lines
cfg.psram_trans_align = 64;             // PSRAM alignment
```

### **⚠️ Performance Considerations**
**Post-Modularization Performance Notes** (Priority 1.1 Complete):
- **General responsiveness**: Excellent for typical UI interactions
- **Root causes of past issues**: I2C bus contention (RTC + IO Expander), LVGL object update frequency
- **Optimization approach**: I2C consolidation into `i2c_manager` (see I2C Bus Architecture above)
  improved performance. The IMU (QMI8658) is **not** removed — it's on the shared I2C bus and actively
  used for compass tilt compensation; GPS is a UART peripheral, so there was never bus contention
  between the two. An earlier version of this note claimed otherwise; that was wrong.

**Performance Trade-offs Made**:
- Modular architecture prioritized over maximum real-time performance
- Template maintainability chosen over edge-case optimization
- Clean code organization achieved with minor performance cost

### **❌ Common Pitfalls**
- **Never** initialize WiFi/BLE before display is stable (causes interference)
- **Never** perform I2C operations in tight loops (causes bus errors)
- **Always** hold LCD_CS HIGH after display init (prevent SPI conflicts)
- **Avoid** global variables in vendor code (thread safety issues)

## Serial Console Interface

The project includes a comprehensive serial command system for runtime control and diagnostics:

**Diagnostic Commands:**
```
  help                 - Show this help
  diag wifi on|off     - Enable/disable WiFi scanning
  diag ble on|off      - Enable/disable Bluetooth scanning
  diag freeze on|off   - Freeze/unfreeze LVGL display
```

**Configuration Commands:**
```
  config show          - Show current configuration values
  config display       - Show display-specific parameters
  config timing        - Show timing intervals and delays
  config pins          - Show GPIO pin assignments
  config set <param> <value> - Set configuration parameter (future)
```

**Memory Management Commands:**
```
  memory [stats]       - Show memory statistics
  memory info          - Show memory layout info
  memory report        - Generate memory report
  memory pools         - Show static pool usage
  memory cleanup       - Force cleanup
  memory integrity     - Check heap integrity
  memory leak <cmd>    - Leak detection commands
```

## Project Structure

```
cc-radar/
├── src/                    # Source code
├── include/               # Project headers
├── lib/                   # Private libraries
├── test/                  # Unit tests
├── partitions/            # Custom partition schemes
├── docs/                  # Component documentation
├── CLAUDE.md             # This file (technical documentation)
├── README.md            # Project overview (for new developers)
├── ROADMAP.md           # Planned features and active tasks
├── CHANGELOG.md         # Complete implementation history
└── platformio.ini        # PlatformIO configuration
```

## Component Documentation

Detailed component documentation is available in the `docs/` directory:

- [`docs/configuration.md`](docs/configuration.md) - Central configuration system and hardware variants
- [`docs/display.md`](docs/display.md) - ST7701 display configuration, timing, and rotation
- [`docs/touch.md`](docs/touch.md) - CST820 touch controller integration
- [`docs/waypoint_filtering.md`](docs/waypoint_filtering.md) - Waypoint filtering system and off-screen indicators
- [`docs/beacon_direction_finding.md`](docs/beacon_direction_finding.md) - can we tell the direction to the beacon? (yes, via body-shadow DF)
- [`docs/i2c.md`](docs/i2c.md) - I2C bus management and device communication
- [`docs/memory_management.md`](docs/memory_management.md) - Advanced memory management system guide
- [`docs/peripherals.md`](docs/peripherals.md) - RTC and GPS integration guides
- [`docs/wifi_implementation_guide.md`](docs/wifi_implementation_guide.md) / [`docs/wifi_power_management.md`](docs/wifi_power_management.md) - WiFi implementation and power patterns (BLE: see Beacon Proximity below)
- [`docs/standby_mode.md`](docs/standby_mode.md) - Low-power standby: entry/wake, power settings, thread-safety history
- [`docs/troubleshooting.md`](docs/troubleshooting.md) - Common issues and solutions
- [`docs/documentation_standards.md`](docs/documentation_standards.md) - Full documentation process (see also Documentation Standards below)

## Development Environment

- **Recommended IDE**: VSCode with PlatformIO IDE extension
- **Debugging**: Hardware debugging supported via PlatformIO debugger
- **Serial Communication**: USB CDC (no external USB-to-serial required)
- **Upload Speed**: 460800 baud (can increase to 921600 if stable)

## Important Build Flags

```cpp
-DLV_CONF_INCLUDE_SIMPLE       // LVGL simple configuration
-Iinclude                      // Project headers
```

USB CDC is configured via `sdkconfig.defaults`, not a build flag — the old Arduino-framework flags
(`-DARDUINO_USB_MODE=1`, `-DARDUINO_USB_CDC_ON_BOOT=1`) that used to live here are gone; this is an
ESP-IDF build (see PlatformIO Settings above). Flash is 16MB, running 4MB OTA app slots (see Memory
Layout above) — not the 3MB figure this section previously stated, which was stale.

---

## Battery Monitoring System

**Status**: Complete ✅ | [Complete Guide](docs/battery_monitoring.md)

Visual battery percentage on the radar screen (GPIO4 ADC, ETA6098 charging IC, 1:3 divider), updated
every 5s via the System Task, green/yellow/red at 70%/50% thresholds. **Display offset is `-150px`,
not a smaller value** — `-50px` caused text cutoff and crashes near the circular display boundary.
The display is round; any absolute-position UI element needs enough margin to stay inside the visible
circle, not just inside the square 480×480 framebuffer (the same class of bug FT-09 hit for waypoints —
see ROADMAP.md).

**Serial Commands**: `battery status`, `battery monitor on|off`, `battery history`, `battery raw`

**Full guide** (hardware, display, power, troubleshooting): [`docs/battery_monitoring.md`](docs/battery_monitoring.md).

---

## Standby Mode

**Status**: Complete ✅ | [Complete Guide](docs/standby_mode.md)

Low-power sleep: 4-second GPIO0 hold (or an optional inactivity timeout, Settings > Display > Auto
Sleep) turns backlight off, drops CPU 240MHz→80MHz, disables WiFi/AP, and stops polling touch
entirely, while GPS stays on at a reduced power mode for continuous track logging. Any button press
wakes (touch isn't read during standby, so it can't). **`enterStandby()`/`wakeFromStandby()` must
never be called directly from a button callback** — they make LVGL calls, callbacks run outside
`display_mutex`, and calling them directly is exactly the shipped bug that froze UI_Task after 7+
hours of runtime. Both are routed through `task_manager::queueUIUpdate()`
(`ENTER_STANDBY`/`WAKE_STANDBY`) and only fall back to a direct call if the queue is full.

**Full guide** (power settings, wake sequence and the I2C/touch-controller reset it performs, why an
LVGL overlay instead of a screen switch, and a known cosmetic bug in the standby screen's battery
readout): [`docs/standby_mode.md`](docs/standby_mode.md).

---

## Waypoint Filtering System

**Status**: Complete ✅ | [Complete Guide](docs/waypoint_filtering.md)

Dual-strategy filtering against clutter: **distance filtering** (shows waypoints within 100× the
current zoom radius, raised from 10× 2026-08-07 — adaptive, not a fixed cutoff) and **sector
clustering** (max 8 off-screen indicators, one per compass sector, closest-per-sector — 50 off-screen
waypoints collapse to 8 triangles, not 50). The on/off-screen boundary test is **circular, not
square** — see FT-09 in ROADMAP.md for why that distinction mattered. Read `wpt_us` off the `perf` HUD
for current cost rather than trusting a fixed estimate — a "<2ms" figure once quoted here was never
actually measured. Off-screen indicators are tappable (2026-08-07) — opens the same waypoint detail
screen an on-screen dot would, showing a one-shot live distance (meters/km via Haversine). **Fixing**
a waypoint (on- or off-screen) shows a separate, continuously-updating distance readout + "locked on"
icon — auto-releases past `RadarConfig::FIXED_WAYPOINT_MAX_DISTANCE_M` (100km), a safety net, not a
normal-use limit.

**Full guide**: [`docs/waypoint_filtering.md`](docs/waypoint_filtering.md).

---

## Waypoint Memory Layout

**Status**: Complete ✅ (2026-07-31) | [ADR-0001](docs/adr/0001-waypoint-detail-psram-cache.md)

`Waypoint::desc`/`hint` live in a `WaypointDetail` block allocated once in **PSRAM**
(`heap_caps_calloc(..., MALLOC_CAP_SPIRAM)` in `ui_manager::init()` — never a section attribute,
`.ext_ram_noinit` boot-crashes on this IDF since constructors don't run for objects placed there);
`Waypoint::desc`/`hint` are pointers into it, `nullptr`-guarded against a failed allocation.

**⚠️ `sizeof(wp.desc)` is now `sizeof(char*)` (8), not the old buffer size.** Any code touching these
fields must use `WaypointDetail::DESC_SIZE`/`HINT_SIZE` explicitly — this is a bitten-once footgun, not
a hypothetical one.

**Full reasoning** (the ~64KB SRAM-and-flash saving, and how the flash saving was confirmed via a
`readelf` section diff rather than assumed): [ADR-0001](docs/adr/0001-waypoint-detail-psram-cache.md).

**Code References**: struct + allocation in `include/ui/ui_manager.h` / `src/ui/ui_manager.cpp`
(`init()`); write site `src/gpx/gpx_loader.cpp` (waypoint commit on `</wpt>`); read site
`src/ui/waypoint_screen.cpp` (`open()`).

---

## Waypoint Two-Tier Index

**Status**: Implemented and field-verified on hardware 2026-08-05 | [ADR-0023](docs/adr/0023-two-tier-waypoint-index.md) | [Design doc](docs/waypoint_two_tier_index_plan.md)

`MAX_WAYPOINTS` (200) is a **working-set** size, not a cap on how many waypoints the device knows
about. Every waypoint across every GPX file is indexed in PSRAM (`gpx_index`, up to 8192 lightweight
entries), and `ui.waypoints[]` holds the 200 closest to the user's actual position — reselected via a
**delta** update (`gpx_loader::reselect()`, System Task, >150m movement) that only re-parses slots
whose occupant actually changed, not the whole set. Skipped while the waypoint detail screen is open,
so a slot can't be recycled out from under it.

**Concurrency note, still relevant**: `ui.waypoints[]` now has two writers (UI Task taps, System Task
reselect), so three UI-Task write sites picked up `ui_state_mutex` protection. Render-path *reads*
remain unprotected — a deliberate, narrow risk accepted in ADR-0023, not an oversight.

**Still open** (unresolved, not just historical): the automatic GPS-driven reselect trigger hasn't been
observed firing from real non-injected movement, and concurrent SD access during a reselect is
unverified — no SD-access mutex exists anywhere in this codebase.

**Full detail** (selection algorithm, measured SRAM/PSRAM cost, field-verification methodology against
an independent Haversine oracle): [ADR-0023](docs/adr/0023-two-tier-waypoint-index.md).

**Code References**: `include/gpx/gpx_index.h`/`src/gpx/gpx_index.cpp` (index); `src/gpx/gpx_loader.cpp`
(`selectAndMaterialize()`, `reselect()`); `src/utils/task_manager.cpp` (movement trigger);
`src/utils/diagnostics.cpp` (`gpx index list/reselect/gentest` debug commands).

---

## Navigation Modes System

**Status**: Complete ✅ | [Complete Guide](docs/navigation_modes.md)

Dual-mode: **heading-up** (default — radar rotates so the user always moves "forward") and
**north-up** (fixed orientation). Solves the reported "when I turn left the radar moved left"
disorientation.

**Heading Source — the compass (QMC5883L), not GPS.** GPS heading fusion was removed entirely; the
compass is read at 10Hz and is the sole source of `ui.current_heading`. GPS still supplies *position*
(UBX NAV-PVT), not heading. **There is no stationary fallback/timeout anymore** — a compass works
standing still, so the old "10s then revert to north-up" behavior this project once had doesn't exist
in current code (two struct fields survive as dead leftovers — see the doc for which ones, don't
resurrect logic around them without checking first).

**Full guide** (rotation math, settings persistence, code references): [`docs/navigation_modes.md`](docs/navigation_modes.md).

---

## Beacon Proximity System

**Status**: Complete ✅ | [Complete Guide](docs/beacon_proximity.md)

BLE item finder, active at 50m zoom only: cyan arc gauge + buzzer sonar (tempo continuous and linear
in dBm, not zoned — RSSI ≈ C − 20·log₁₀(d), so equal dBm steps are equal distance ratios) as a
configured beacon MAC gets closer. NimBLE stack, ~25KB SRAM.

**Three load-bearing warnings, easy to re-break**:
- **The tag must advertise in Legacy (BLE 4.0) mode.** Extended Advertisement/PHY Coded (BLE 5.0) is
  completely invisible — indistinguishable from a dead tag (-127 dBm, silent). `beacon status`
  disambiguates: high `Scan callbacks` with `0 matched target` means the scan is healthy and the tag
  is the problem.
- **Passive scanning is load-bearing, not a power choice** — active scanning defers NimBLE's
  `onResult` callback until the scan response arrives for a legacy `ADV_IND` advertiser; passive
  delivers on the advertisement itself.
- **`setAdvertisedDeviceCallbacks(cb, wantDuplicates)` silently re-enables the duplicate filter** if
  called after an explicit `setDuplicateFilter(false)` — this exact footgun already broke
  `debugScanAll()` once.

**Beacon has absolute priority over the waypoint sonar** — `isInRange()` releases any fixed waypoint
outright rather than just yielding the buzzer, since a fix would otherwise re-take the sonar the
moment the beacon dipped out of range.

**Settings**: target MAC, measured power (dBm @ 1m), path loss exponent — NVS-persistent (`bcn_*`).
**Serial**: `beacon status|scan|test|zone|trend`.

**Direction finding** ("which way do I walk?") is unblocked (was rate-limited at 2Hz, now 4.24-4.37Hz)
but not yet built — body-shadow DF via the compass, since true BT 5.1 AoA needs hardware this board
doesn't have. Design: [`docs/beacon_direction_finding.md`](docs/beacon_direction_finding.md).

**Full guide** (RSSI EMA/τ derivation, zone thresholds, the 2Hz→4.24Hz rate-starvation fix, code
references): [`docs/beacon_proximity.md`](docs/beacon_proximity.md).

---

## Render Pipeline

**Status**: Optimized ✅ | [Backlog + measurements](docs/performance_optimization_backlog.md)

Frame time went **~499ms → ~94ms (~0.8 → ~10 fps)**. The radar no longer uses an `lv_canvas`, and
the flush no longer copies anything.

**Current architecture**:
- **`radar_obj`** — a plain `lv_obj` that paints itself from an `LV_EVENT_DRAW_MAIN` handler
  (`navigation::radarDrawEventCb`), emitting geometry straight into LVGL's draw context. There is no
  intermediate image buffer.
- **`updateRadarDisplay()` does not paint.** It refreshes HUD label widgets, runs the waypoint sonar,
  and calls `lv_obj_invalidate()`. Geometry happens later, inside LVGL's refresh.
- **Background is a style** (`bg_color` on `radar_obj`), painted by LVGL's class draw handler before
  the user `DRAW_MAIN` callback — user callbacks run after `lv_obj_event_base` (`lv_event.c`,
  `event_send_core`).
- **90° rotation is a tiled transpose** in `lvgl_flush_cb`, not LVGL's `sw_rotate`
  (`device_manager.cpp`, `rotate90_tiled`). It transposes via a 2KB internal-SRAM scratch tile so
  both PSRAM streams stay sequential. Runtime-switchable with `rot on|off|tiled`.
- **The panel has two framebuffers** (`num_fbs = 2`). The transpose writes directly into the back
  one; `esp_lcd_panel_draw_bitmap` recognises its own framebuffer and swaps `cur_fb_index` instead of
  copying. The flush costs **0.02ms** — there is no full-screen memcpy in the pipeline.

**Four constraints that are load-bearing — do not "clean these up"**:

1. **`clip_corner` must stay OFF on the radar stage** (`ui_manager.cpp`). LVGL answers
   `LV_EVENT_COVER_CHECK` with `LV_COVER_RES_MASKED` for any object with `clip_corner`, and
   `lv_refr_get_top_obj` treats `MASKED` as *stop, do not descend*. Turning it on makes LVGL repaint
   the screen and stage backgrounds beneath the radar every frame (**+61 ms**) and installs a radius
   mask that every child draw call blends through (**grid 3× slower**). The panel is physically
   round, so the clipping it provides is invisible anyway.

2. **`radar_obj` must not be `CLICKABLE`.** `lv_obj_create()` sets the flag by default
   (`lv_obj.c:436`); `lv_canvas_create()` did not. With it set, the radar surface wins hit-testing
   and swallows presses before they reach the stage handler calling `handleTapAt()` — waypoint
   detail taps silently stop working while the display looks perfect.

3. **`full_refresh` must stay `1` whenever the zero-copy path is active**, and `0` otherwise. The
   transpose rewrites the entire back framebuffer; a partial flush area would leave the rest of it
   holding a two-frames-old image. It is set from the rotation mode in both `initLVGL()` and
   `applyPendingRotMode()` and must move with it, because LVGL rejects `full_refresh` together with
   `sw_rotate` — so `rot on` has to clear it.

4. **The `on_frame_buf_complete` guard is not dead code.** The driver latches
   `bb_fb_index = cur_fb_index` only at a frame boundary, so between a swap and that latch the "back"
   buffer is still being scanned out. At 94ms/frame against a 26.6ms panel period it never blocks —
   it exists so that raising PCLK or shaving the frame further cannot silently reintroduce tearing.

**Timing semantics**: painting runs *inside* the LVGL refresh, so `paint_us` is a component of
`refr_ms`, not sequential with it. **Frame = `label_us + refr_ms`** — adding `paint` double-counts.
Read via the `perf` serial command or the DEV tab.

**Where the 85ms sits** (measured at 240MHz, 2026-07-28; 160MHz values in parens): rotate 38.3
(47.4) + non-radar LVGL draw 17.0 (23.2) + radar bg fill 20.5 (21.5) + radar paint 9.3 (9.4).

**"Full-screen PSRAM write" is not one category** — the 240MHz measurement split the two apart:
- **radar bg fill scaled 1.05×** — genuinely at the bus ceiling, as a plain optimized `memcpy` over
  the same bytes (~27 MB/s) predicted.
- **rotate scaled 1.24×**, the largest absolute gain of any stage. ~24% of the transpose was CPU
  work — loop overhead and the scatter into the SRAM tile — not bandwidth. An earlier version of this
  section claimed both writes "sit near the memory ceiling"; that was true of only one of them.
- **radar paint scaled 1.01×** — it did *not* move. Emitting geometry into LVGL's draw context is
  bound by writing the draw buffer, not by computing the geometry, so optimizing the drawing math
  would buy nothing.

**Clocks** (all measurements above were taken at 160MHz):
- **CPU is now 240MHz**, set by `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240` in `sdkconfig.defaults`. It had
  been at 160 since the ESP-IDF migration — vanilla IDF's default, where the Arduino core used to set
  240 for us. Verified on hardware: **frame 101.5 → 85.2ms (1.19×)**. Boot prints the measured value;
  if it ever says 160 again, the generated `sdkconfig.cc-radar` is stale (see below).
- **PCLK is still 10MHz.** Raising it is the item where the `on_frame_buf_complete` guard stops being
  theoretical: it now has ~3.2× of margin at 85ms/frame against a 26.6ms panel period, and a higher
  PCLK shortens that period.

**`sdkconfig.defaults` is not enough on its own.** PlatformIO does *not* regenerate
`sdkconfig.cc-radar` when `sdkconfig.defaults` changes — a build will succeed and silently keep the
old setting. Delete `sdkconfig.cc-radar`, rebuild, and diff against the previous copy: the committed
file had accumulated three settings that diverged from the defaults, one of which
(`CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL`) existed *only* in the generated file and would have
silently reverted.

**Sensor rate**: `SYSTEM_UPDATE_MS = 100` is the sensor clock — it drives *both* the compass sub-timer
(20ms gate) and the GPS gate (`GPS_UPDATE_INTERVAL_MS = 100`), so compass and GPS both sample at 10Hz.
Safe only because render requests are coalesced to at most one per UI Task loop.

With a GPS fix, `RADAR_REFRESH` is queued every sample, so the UI Task renders nearly every loop and
polls button/touch once per ~90ms rather than once per 26.6ms vsync. **Verified fine outdoors** — 90ms
is shorter than a real button press (132–186ms), so nothing is missed.

**Possible future revisit**: if Core 1 ever needs relief — a higher PCLK, or a much heavier waypoint
load — dropping the *GPS-driven* `RADAR_REFRESH` to 5Hz while leaving the compass at 10Hz would halve
the render rate for little visible cost, since translation matters less than rotation. Not needed
today. If it is ever done, lower the GPS refresh and **not** the compass rate — the compass rate is
what makes the rotation feel right.

**Methodology note**: three separate estimates in the backlog were wrong because a *residual*
(`total − known`) was named after a hypothesis. Never attribute an un-instrumented remainder; bracket
it with `esp_timer_get_time()` first. See "The residual trap" in the backlog.

**⚠️ The backlog is no longer render-only.** It was scoped to a single number — frame time — and every
other subsystem went unexamined until a 2026-07-31 audit. Both items it turned up have since been
built, and **neither was a CPU problem**:
- ~~**§7 — beacon proximity** is rate-starved at 2 Hz against a 5 Hz source.~~ ✅ fixed — see the
  Beacon Proximity section above.
- ~~**§8 — sonar/buzzer**~~ ✅ **fixed.** The walking beat grid (`= now + interval` instead of `+=`,
  ~8% jitter and a ~4% flat tempo) is fixed and verified. The waypoint distance→tempo mapping is now
  **continuous** — geometric, 2000 ms at 50 m → 250 ms at 2 m — with a τ = 1.5 s EMA on the *distance*
  as the GPS-noise guard, replacing the four-zone ladder and its hysteresis outright. The waypoint
  sonar also honours `Waypoint::found` now, so tapping the waypoint within 15 m silences it exactly
  as tapping the beacon ball does.

§8 also grades the rest: input latency adequate for taps and coarse for drags; GPS healthy bar a
syscall-per-byte UART drain; **compass and battery healthy, no action.** The compass is the example to
follow — when its rate went 5→10 Hz someone correctly re-derived the heading EMA (1.5° → 0.5°). The
recurring defect everywhere else is *a rate or quantization constant nobody re-derived after the
pipeline around it changed*.

**A pattern worth generalising, from the sonar fix**: prefer a *continuous* mapping for a continuous
physical quantity, and put the noise filter on the **input** (a τ-based EMA on distance/RSSI) rather
than hysteresis on the **output**. Hysteresis is only correct where a genuinely discrete decision is
being made — beeping vs silent, in range vs out. The beacon ring (§7.3c) wants the same treatment.

**Known, accepted issue: every GPX upload/delete visibly glitches the display** (shifted/wrapped
frame content) — a real FFat write/erase briefly disables both cores' cache/interrupts, which can
starve the RGB panel's DMA refill. `CONFIG_SPI_FLASH_AUTO_SUSPEND` was enabled to test as a fix
(2026-08-07) but field data suggests it didn't resolve it; not pursued further, documented in the
GPX web manager UI instead of the render pipeline. Detail: `docs/adr/0028-defer-gpx-reload-to-
explicit-endpoint.md`'s Verification status section, CHANGELOG.md.

---

## Documentation Standards

**IMPORTANT**: This project prioritizes thorough documentation. Every significant implementation must be documented properly.

**Key Principles**:
- **Major features** require CHANGELOG.md entry + component doc (`docs/*.md`)
- **Critical fixes** documented with root cause, solution, and impact
- **Architectural decisions** (a choice between real alternatives, not just "what we built") get an
  ADR in `docs/adr/` — see below
- **Build impact** always measured (flash/RAM changes)
- **Code references** include file:line citations
- **Templates** ensure consistency (CHANGELOG, component docs, ADR)
- **ROADMAP.md stays summary-only** — it's the plan, not the history. An entry gets a symptom/root
  cause/status in a few sentences and a link to CHANGELOG.md or the relevant `docs/*.md`; the full
  writeup (build impact, field-test notes, reasoning) lives there, not duplicated in ROADMAP.md. If a
  ROADMAP entry is growing past that, the detail belongs in CHANGELOG.md instead.

**Always Document**:
- New features or subsystems
- Critical bug fixes
- Architectural changes
- Hardware integration
- User-facing improvements
- Performance optimizations
- Build system changes

**Architecture Decision Records** (`docs/adr/`, started 2026-07-31):
CHANGELOG.md and component docs capture *what* was built and *why it works*; an ADR captures *why
this option and not the others* — for decisions that were genuinely reversible another way, where a
future reader will otherwise wonder "why didn't they just—". Use the template at
`docs/adr/0000-template.md`, numbered sequentially (`0001-`, `0002-`, ...). Not every change needs
one — a bug fix or a tuned constant doesn't; a choice like "software rotation vs hardware", "passive
vs active BLE scan", "zero-copy `lv_obj` draw vs `lv_canvas`" does. When in doubt, check the backlog
docs (`performance_optimization_backlog.md`, `ROADMAP.md`) first — several existing decisions
documented there (the four load-bearing render flags, the `I2C_PROCESS_MS` floor, passive-vs-active
BLE scanning) are ADR candidates once someone has time to extract them; new decisions from this point
forward should get one directly instead of only living in prose docs.
**Historical backfill** (decisions made before 2026-07-31) is a separate, lower-priority pass — not
required for new work — tracked in `docs/adr/BACKFILL_PLAN.md`.

**Documentation Flow**:
1. **During**: Add one-line to CHANGELOG.md immediately
2. **After**: Expand with technical details, build impact, user benefits
3. **Component docs**: Create `docs/*.md` for major features
4. **ADR**: Add `docs/adr/NNNN-title.md` if the change was a decision between real alternatives
5. **ROADMAP.md**: Move/update the entry's status (Known Issue → Planned → Resolved), summary-only,
   linking to the CHANGELOG.md entry or component doc for detail — never restate it
6. **CLAUDE.md**: Update if architecture changed (brief summary + link to docs)
7. **README.md**: Update features list if user-visible

**Quick Checklist** (after significant work):
- [ ] CHANGELOG.md entry
- [ ] Build impact measured
- [ ] Component doc created/updated (if needed)
- [ ] ADR added (if a real alternative was rejected)
- [ ] ROADMAP.md status updated (summary-only, link to detail)
- [ ] CLAUDE.md updated (if architecture changed)
- [ ] README.md updated (if user-visible)

**CLAUDE.md Size Discipline**:
CLAUDE.md is the fast-orientation file loaded into every session — it should hold evergreen
quick-reference (build commands, pin maps, hardware init order, common pitfalls) and *currently
load-bearing* constraints (things that will silently regress if "cleaned up" — e.g. the four
render-pipeline flags, `UI_PRIORITY`, `I2C_PROCESS_MS`). It should **not** hold feature narrative,
measurement history, or anything a `docs/*.md`/ADR "Complete Guide" link already covers — that content
belongs at the link, not duplicated above it.
- **Audit trigger**: whenever this file crosses ~500 lines, or every ~3 months, whichever comes first
  — do a pass over every section asking "does a `docs/*.md` or ADR already exist for this, or should
  one?" If yes, cut the section to a 5–10 line summary + link. If a section is marked
  superseded/historical, delete it outright — the history lives in CHANGELOG.md, not here.
- **The trailing "Last updated" line is the usual offender** — it accretes into a mini-changelog over
  time. Keep it to one line (date + a pointer to CHANGELOG.md); don't append narrative to it going
  forward.
- **Never trim an explicitly load-bearing constraint to hit a line-count target.** Moving the
  *narrative* around a "don't clean this up" statement is fine; the constraint itself must survive
  intact somewhere in this file even if that means landing above the target line count.

**Complete Documentation Guide**: [`docs/documentation_standards.md`](docs/documentation_standards.md)

---

*This document serves as the master reference for ESP32-S3 Touch LCD projects. Keep it updated as the architecture evolves.*

**Last updated**: 2026-08-07 — see [CHANGELOG.md](CHANGELOG.md) for the full history.
