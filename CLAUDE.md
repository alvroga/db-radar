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

### **✅ ENHANCED CONFIGURATION SYSTEM** (Priority 2.4 Complete)

The project now features a comprehensive configuration management system that consolidates all magic numbers and provides runtime control via serial commands.

**Central Configuration** (`include/core/system_config.h`):
- **Hardware Variants**: Support for multiple display configurations (480x480, 320x240, 240x320)
- **Organized Namespaces**: display, pins, communication, timing, ui, backlight, memory, features
- **Compile-time Validation**: Static assertions ensure configuration consistency
- **Easy Customization**: Change one header to adapt entire project

```cpp
// Hardware variant selection
#define HARDWARE_WAVESHARE_ESP32_S3_TOUCH_LCD_2_1  // Default: 2.1" 480x480

namespace system_config {
    namespace display {
        constexpr int SCREEN_WIDTH = 480;
        constexpr int SCREEN_HEIGHT = 480;
        constexpr int PCLK_HZ = 10000000;      // 10MHz - proven stable
    }

    namespace timing {
        constexpr uint32_t WIFI_SCAN_INTERVAL_MS = 15000;    // WiFi scan every 15 seconds
        constexpr uint32_t BLE_SCAN_INTERVAL_MS = 10000;     // BLE scan every 10 seconds
        constexpr uint32_t RTC_READ_INTERVAL_MS = 5000;      // RTC read every 5 seconds
    }

    namespace features {
        constexpr bool ENABLE_WIFI_SCANNING = true;
        constexpr bool ENABLE_BLE_SCANNING = true;
        constexpr bool ENABLE_DEBUG_LOGGING = true;
    }
}
```

**Runtime Configuration** (via serial commands):
- `config show` - Display all current configuration values
- `config display` - Show display-specific parameters
- `config timing` - Show timing intervals and delays
- `config pins` - Show GPIO pin assignments
- Future: Runtime parameter changes with SD card persistence

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

**Performance Improvements Achieved:**
- ✅ **Zero UI freezes** - Legacy blocking I2C operations eliminated
- ✅ **Improved responsiveness** - UI runs on dedicated Core 1 with highest priority
- ✅ **System stability** - No more heap corruption or reboots
- ✅ **Modular architecture** - Template-level solution for ESP32-S3 projects

**Critical Fix Applied:**
The legacy `timer1sCallback` in navigation.cpp was performing direct I2C operations (RTC reads every 5 seconds) that created bus contention. This has been completely eliminated in favor of the queued task system, with RTC updates now occurring every 30 seconds through proper I2C task requests.

**Backwards Compatibility:**
The system includes a fallback mode that can switch to legacy loop-based architecture if needed, ensuring project compatibility while providing the benefits of advanced multitasking.

### **⚠️ OPTIMIZED DISPLAY PERFORMANCE** (Priority 3.6 — SUPERSEDED, kept as history)

> **Read the Render Pipeline section near the end of this file instead.** This section predates the
> ESP-IDF migration and the 2026-07-28 render work, and three of its claims are now false:
> - *"ESP-IDF version doesn't support bounce buffer"* — **wrong.** A 10-line SRAM bounce buffer is
>   configured and active (`system_config.h` `BOUNCE_BUFFER_LINES = 10`, 18.75KB;
>   `device_manager.cpp` `cfg.bounce_buffer_size_px`).
> - *"`full_refresh = 0` / partial refresh"* — **inverted.** It is `1` in the default TILED rotation
>   mode and must be, or the transpose leaves stale pixels. See load-bearing constraint #3.
> - *"software rotation"* — replaced by a tiled transpose in the flush callback; LVGL `sw_rotate` is
>   off in the default mode.
>
> What is still true: the 10MHz PCLK boundary, `BUFFER_LINES = 480`, and the tearing analysis.

The project now features **enhanced display performance** through careful optimization while maintaining the rock-solid stability of the proven 10MHz PCLK timing. Comprehensive testing revealed critical stability boundaries that guided the final optimization approach.

**Display Configuration Optimizations** (`include/core/system_config.h` and `src/core/device_manager.cpp`):
- **Full-Frame LVGL Buffers**: Optimized from 40 → 50 → 120 → 160 → **480 lines** (full frame — eliminates transition wipe artifact with software rotation)
- **Partial Refresh for Performance**: Enabled selective screen updates for fast rendering
- **Critical Timing Preserved**: 10MHz PCLK maintained as the proven stable frequency

**Key Findings from Optimization Testing:**
- **10MHz PCLK is Critical**: Testing 12MHz caused screen jitter - this timing is hardware-specific and sacred
- **Extra-Large Buffers Minimize Tearing**: 160-line buffers reduce flush operations to just 3 per frame
- **Partial Refresh is Faster**: LVGL `full_refresh = 0` only redraws changed areas (much faster than full screen)
- **Buffer Size vs Tearing**: Fewer flush operations = fewer visible tearing artifacts
- **Hardware Limitation**: ESP-IDF version doesn't support bounce buffer - large buffers are best alternative
- **Critical Path Sensitivity**: Even simple operations in the flush callback can cause performance regression

```cpp
// Final optimized display configuration
namespace display {
    constexpr int PCLK_HZ = 10000000;      // 10MHz - proven stable (critical)
    constexpr int BUFFER_LINES = 480;      // Full-frame buffer — 1 flush per frame, eliminates rotation wipe artifact
}

// LVGL driver optimization
disp_drv.full_refresh = 0;          // Partial refresh for fast rendering
```

**Performance Improvements Achieved:**
- ✅ **Full-frame buffers** - Complete frame rendered before flush, eliminates screen wipe during transitions
- ✅ **1 flush per frame** - Maximum efficiency (was 3 at 160 lines, 10 at 50 lines)
- ✅ **Transition artifacts eliminated** - Software rotation wipe artifact gone with full-frame buffers
- ✅ **Maintained stability** - Zero jitter or visual artifacts
- ✅ **Template-ready optimization** - Demonstrates proper ESP32-S3 display tuning methodology

**Memory Trade-off**:
- Buffer size: 921KB PSRAM (480 lines × 480 pixels × 2 bytes × 2 buffers)
- Impact: +829KB over 50-line configuration (11.5% of 8MB PSRAM — still 85%+ free with all features active)

**Screen Tearing Root Cause and Limitations**:
- **Problem**: Display DMA scans pixels from PSRAM while LVGL writes new pixels (asynchronous)
- **Ideal Solution**: Hardware bounce buffer (SRAM staging area) - NOT supported in this ESP-IDF version
- **Practical Solution**: Extra-large LVGL buffers (160 lines) minimize visible tearing
- **Limitation**: Cannot completely eliminate tearing without VSYNC callback or hardware bounce buffer
- **Trade-off**: Larger buffers use more PSRAM but provide smoother visual experience

**Critical Optimization Lessons:**
- **Hardware-specific timing constraints** must be respected (10MHz PCLK boundary)
- **Incremental testing approach** prevents regressions and isolates problematic changes
- **User perception validation** is essential - even microsecond optimizations can cause noticeable regression
- **Critical path optimization** requires extreme care in interrupt-driven callbacks

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
- **Custom Partitions**: `partitions/partitions_ota.csv` — 2 × 2MB OTA app slots + ~11.7MB FFat.
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

**Physical Orientation**: 90° CCW (counter-clockwise) rotation due to enclosure design
**Software Compensation**: 90° CW (clockwise) LVGL rotation to make UI appear upright

The display is physically rotated 90° CCW in the enclosure. LVGL automatically compensates with a 90° CW software rotation, ensuring the UI appears correctly oriented to the user.

**Configuration** (`include/core/system_config.h`):
```cpp
namespace display {
    constexpr int ROTATION_DEGREES = 90;   // 90° CW software rotation
}
```

**Implementation** (`src/core/device_manager.cpp:453-477`):
```cpp
// Enable software rotation BEFORE registration (critical for RGB panels)
#if LV_VERSION_CHECK(8,0,0)
    if (system_config::display::ROTATION_DEGREES == 90) {
        disp_drv.sw_rotate = 1;              // Enable software rotation
        disp_drv.rotated = LV_DISP_ROT_90;  // 90° CW rotation
        Serial.println("[LVGL] Software rotation enabled: 90° CW");
    }
#endif

lv_disp_t* disp = lv_disp_drv_register(&disp_drv);
```

**Critical**: For ESP32 RGB panels, rotation must be configured **before** calling `lv_disp_drv_register()`. Post-registration rotation (`lv_disp_set_rotation()`) only affects touch input, not graphics.

**LVGL Automatic Handling**:
- ✅ All UI elements rotated automatically
- ✅ Touch input coordinates transformed automatically
- ✅ No application code changes needed
- ✅ Zero performance impact (hardware-accelerated)

**Testing**: After upload, verify touch alignment by tapping screen corners and testing UI interactions.

### **I2C Bus Architecture**
**✅ UNIFIED I2C MANAGER** (Priority 1.2 Complete)

The project now uses a **unified `i2c_manager.cpp`** that consolidates all I2C operations with enterprise-grade error handling.

**Shared I2C Bus** (SDA=15, SCL=7) @ 400kHz hosts multiple devices:
- Touch Controller (CST820) @ 0x15
- RTC (PCF85063) @ 0x51
- IO Expander (TCA9554) @ 0x20

**I2C Manager Features:**
- **Device handle abstraction** - Type-safe device management
- **Automatic retry logic** - Configurable retries with intelligent delays
- **Error reporting** - Detailed logging with device names and register info
- **Statistics tracking** - Monitor I2C health and performance
- **Vendor compatibility** - Legacy I2C_Driver.cpp redirects to unified manager

```cpp
// Modern I2C usage
#include "i2c_manager.h"

// Read from device with automatic retries
uint8_t data;
bool success = i2c_manager::readByte(i2c_manager::RTC_DEVICE, 0x01, data);

// EXIO operations (TCA9554)
i2c_manager::exio::State exio_state;
i2c_manager::exio::begin(exio_state);
i2c_manager::exio::set(i2c_manager::exio::BUZZER, false, exio_state);
```

**Legacy Elimination:**
- ❌ **Removed**: `TCA9554PWR.cpp` (5044 bytes dead code)
- ❌ **Removed**: `exio.cpp` (absorbed into i2c_manager)
- ✅ **Modernized**: `I2C_Driver.cpp` (now compatibility layer)

**🚨 Critical Fix Applied:**
During consolidation, a critical pin mapping error was discovered and fixed:
```cpp
// WRONG (caused buzzer stuck ON):
BUZZER = 0, LCD_CS = 1, LCD_RST = 2

// CORRECT (matches hardware):
LCD_RST = 0, TP_RST = 1, LCD_CS = 2, BUZZER = 7
```
This fix resolved system boot failures and hardware control issues.

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

### **✅ PROFESSIONAL MEMORY MANAGEMENT** (Priority 2.3 Complete)

The template includes enterprise-grade memory management that prevents crashes, detects leaks, and provides powerful debugging tools.

**Key Features:**
- **Real-Time Monitoring**: Tracks heap, PSRAM, LVGL, and DMA memory usage with statistics
- **Memory Pools**: Ultra-conservative fixed-size pools (2×256B + 2×128B = 768 bytes) for fragmentation prevention
- **Automatic Health Checks**: Periodic heap corruption detection and low-memory warnings
- **Diagnostic Interface**: Complete serial command system for memory analysis and debugging
- **Crash Prevention**: Robust error handling prevents memory-related system failures

**Available Commands:**
```bash
memory stats          # Current memory statistics
memory info           # Memory layout information
memory report         # Comprehensive system report
memory pools          # Pool usage and statistics
memory cleanup        # Force cleanup (screens+LVGL)
memory integrity      # Check heap integrity
memory leak start/stop/report  # Leak detection tools
memory stress         # Comprehensive stability test
```

**For Developers:**
- Debug memory issues in real-time during development
- Catch memory leaks before they reach production
- Understand memory usage patterns of different features
- Advanced tools for embedded system optimization

**For End Users:**
- Rock-solid stability - no random freezes or reboots
- Long-term reliability for 24/7 operation
- Responsive UI performance without memory fragmentation
- Commercial-grade reliability for production products

**Recovery Story:**
The system was initially designed with larger pools (12KB+) which caused boot loops. Through careful analysis, we implemented ultra-conservative settings (768 bytes) that provide all benefits with perfect stability. This demonstrates proper embedded memory management - conservative, safe, and scalable.

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
- **Optimization approach**:
  - Priority 1.2 (I2C consolidation) improved performance
  - IMU removed to eliminate I2C bus contention with GPS

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
- [`docs/display.md`](docs/display.md) - ST7701 display configuration and timing
- [`docs/touch.md`](docs/touch.md) - CST820 touch controller integration
- [`docs/waypoint_filtering.md`](docs/waypoint_filtering.md) - **NEW**: Waypoint filtering system and off-screen indicators
- [`docs/beacon_direction_finding.md`](docs/beacon_direction_finding.md) - **NEW**: can we tell the direction to the beacon? (yes, via body-shadow DF — blocked on BLE rate work)
- [`docs/i2c.md`](docs/i2c.md) - I2C bus management and device communication
- [`docs/memory_management.md`](docs/memory_management.md) - Advanced memory management system guide
- [`docs/peripherals.md`](docs/peripherals.md) - RTC, GPS integration guides
- [`docs/connectivity.md`](docs/connectivity.md) - WiFi/BLE implementation patterns
- [`docs/troubleshooting.md`](docs/troubleshooting.md) - Common issues and solutions

## Development Environment

- **Recommended IDE**: VSCode with PlatformIO IDE extension
- **Debugging**: Hardware debugging supported via PlatformIO debugger
- **Serial Communication**: USB CDC (no external USB-to-serial required)
- **Upload Speed**: 460800 baud (can increase to 921600 if stable)

## Memory and Performance

### **Memory Usage**
- **PSRAM**: Used for LVGL framebuffers and large data structures
- **SRAM**: Reserved for time-critical operations and interrupt handlers
- **Flash**: 3MB available for application code

### **Performance Characteristics**
- **Display Refresh**: ~60 FPS with optimized timings
- **Touch Responsiveness**: <50ms latency
- **I2C Operations**: 400kHz bus speed (can reduce to 100kHz if unstable)
- **WiFi Scanning**: Every 15 seconds (async)
- **BLE Scanning**: Every 10 seconds (3-second active scan)

---

## Important Build Flags

```cpp
-DARDUINO_USB_MODE=1           // Enable USB mode
-DARDUINO_USB_CDC_ON_BOOT=1    // Enable USB CDC on boot
-DLV_CONF_INCLUDE_SIMPLE       // LVGL simple configuration
-Iinclude                      // Project headers
```

---

## Battery Monitoring System

**Status**: Complete ✅ | [Complete Guide](docs/battery_monitoring.md)

Visual battery percentage display on radar screen with comprehensive monitoring system.

**Key Architecture**:
- **Two Systems**: Monitoring (GPIO4 ADC sampling) + Display (UI label updates)
- **Display Position**: Top-right at `-150px` offset (critical for circular clipping)
- **Update Rate**: 5-second intervals via System Task
- **Color Coding**: Green (>70%) → Yellow (50-70%) → Red (<50%)

**Hardware**: GPIO4 (BAT_ADC), ETA6098 charging IC, 1:3 voltage divider (R5+R9)

**Serial Commands**: `battery status`, `battery monitor on|off`, `battery history`, `battery raw`

**Critical Implementation Note**: Initial `-50px` offset caused text cutoff ("Ba" instead of full text) and system crashes. `-150px` provides safe margin from circular display boundary.

**Code References**:
- Display: `src/ui/ui_manager.cpp:204-210` - Label creation with safe positioning
- Update: `src/utils/task_manager.cpp:695-714` - System Task battery updates
- Monitoring: `src/hardware/sensors/battery.cpp` - ADC sampling and trend analysis
- Interface: `include/hardware/sensors/battery.h` - BatteryStatus structure

**Reference Documentation**:
- Complete guide: [`docs/battery_monitoring.md`](docs/battery_monitoring.md)
- Power management: [`docs/battery_power_management_implementation.md`](docs/battery_power_management_implementation.md)
- Display summary: [`docs/battery_display_summary.md`](docs/battery_display_summary.md)

---

## Waypoint Filtering System

**Status**: Complete ✅ | [Complete Guide](docs/waypoint_filtering.md)

Dual-strategy intelligent filtering system that prevents visual clutter while maintaining situational awareness.

**Key Architecture**:
- **Strategy 1: Distance Filtering** - Shows waypoints within 10× zoom radius (adaptive threshold)
- **Strategy 2: Sector Clustering** - Maximum 8 directional off-screen indicators (N, NE, E, SE, S, SW, W, NW)

**Distance Examples**:
- 10km zoom → shows waypoints within 100km (allows navigation planning beyond visible area)
- 1km zoom → shows waypoints within 10km
- 100m zoom → shows waypoints within 1km

**Off-Screen Indicators**:
- Orange triangles at screen edge (15px, 20px inset)
- Maximum 8 indicators (one per compass sector)
- Keeps only closest waypoint per sector
- **Result**: 50 waypoints off-screen → only 8 indicators (prevents clutter)

**Performance**: O(n) complexity, 128 bytes stack allocation. The "<2ms for 50 waypoints" figure once
quoted here was never measured — waypoint drawing is ~5ms in the instrumented frame breakdown. Read
`wpt_us` off the `perf` HUD rather than trusting either number.

**Code References**:
- Algorithm: `src/ui/navigation.cpp:291-395` - `drawWaypoints()` function
- Config: `include/ui/ui_manager.h:54-81` - `RadarConfig::DISTANCE_FILTER_MULTIPLIER = 10.0f`
- Off-screen drawing: `src/ui/navigation.cpp:250-289` - `drawOffScreenIndicator()`

**Reference Documentation**: [`docs/waypoint_filtering.md`](docs/waypoint_filtering.md) - Complete technical deep-dive

---

## Waypoint Memory Layout

**Status**: Complete ✅ (2026-07-31) | [ADR-0001](docs/adr/0001-waypoint-detail-psram-cache.md)

`Waypoint::desc`/`hint` (1024B + 256B each, × `MAX_WAYPOINTS`) live in **PSRAM**, not SRAM — they were
the single largest firmware symbol (70,992 B, ~37% of static RAM at the `MAX_WAYPOINTS = 50` this
migration was measured against) despite being read in exactly one place (the detail screen, one
waypoint at a time). `MAX_WAYPOINTS` is now **500** (see [ADR-0022](docs/adr/0022-waypoint-cap-raised-to-500-not-700.md))
— the 70,992 B figure is historical, from before this PSRAM move, not the current PSRAM block size.

```cpp
// include/ui/ui_manager.h
struct WaypointDetail {           // allocated as one PSRAM block, MAX_WAYPOINTS entries
    char desc[1024] = {};
    char hint[256] = {};
};
struct Waypoint {
    // ... lat/lon/valid/found/name/display_name stay in SRAM (hot, small) ...
    char* desc = nullptr;         // points into the PSRAM WaypointDetail block
    char* hint = nullptr;         // nullptr if PSRAM allocation failed — guard before use
};
```

**Allocation**: `heap_caps_calloc(MAX_WAYPOINTS, sizeof(WaypointDetail), MALLOC_CAP_SPIRAM)` in
`ui_manager::init()` — never a section attribute (`.ext_ram_noinit` boot-crashes on this IDF, since
constructors aren't run for objects placed there).

**⚠️ `sizeof(wp.desc)` is now `sizeof(char*)` (8), not 1024.** Any code touching these fields must use
`WaypointDetail::DESC_SIZE`/`HINT_SIZE` explicitly — this bitten-once footgun is why the guard exists.

**Frees ~64KB SRAM — and flash too**, confirmed via a `readelf -S` section diff, not assumed:
`g_ui_state` has non-zero-initialized fields (e.g. `current_zoom`'s default), so the whole object was
placed in `.dram0.data` (a PROGBITS section — its zero bytes are literal zeros stored in flash and
copied to RAM at boot) rather than the free `.dram0.bss`. `.dram0.bss` was byte-identical before/after
the change; the entire saving came out of `.dram0.data`.

This freed the SRAM headroom that made raising `MAX_WAYPOINTS` (50 → 500) affordable — see
[ADR-0022](docs/adr/0022-waypoint-cap-raised-to-500-not-700.md) and ROADMAP.md.

**Code References**:
- Struct: `include/ui/ui_manager.h` - `WaypointDetail`, `Waypoint::desc`/`hint`
- Allocation: `src/ui/ui_manager.cpp` - `init()`
- Write site: `src/gpx/gpx_loader.cpp` - waypoint commit on `</wpt>`
- Read site: `src/ui/waypoint_screen.cpp` - `open()`

---

## Waypoint Two-Tier Index

**Status**: Implemented and field-verified on hardware 2026-08-05, against an independent Haversine oracle (cold selection, forced high-churn reselect, HDOP gating, live end-to-end nearby-waypoints test) | [ADR-0023](docs/adr/0023-two-tier-waypoint-index.md) | [Design doc](docs/waypoint_two_tier_index_plan.md)

`MAX_WAYPOINTS` (200, see above) is a **working-set** size, not a limit on how many waypoints the
device knows about. Every waypoint across every GPX file is indexed separately in PSRAM
(`gpx_index`, new module — `include/gpx/gpx_index.h`/`src/gpx/gpx_index.cpp`, up to 8192 lightweight
`{lat, lon, file_offset, file_id, found}` entries), and `ui.waypoints[]` holds the 200 closest to the
user's actual position, not whichever 200 happened to load first in filesystem order.

**Selection**: `gpx_loader::selectAndMaterialize()` — Haversine (`utils/geo.h`, deliberately *not* the
equirectangular approximation `navigation.cpp` uses for rendering, since index candidates can be
globally distributed) + `std::partial_sort` over the PSRAM index, then `fseek` + re-parse only the
winning entries, grouped by file to minimize SD reopens.

**Kept current as the user moves**: `gpx_loader::reselect()`, called from the System Task
(`task_manager.cpp::updateStatusLabels()`) when GPS has moved >150m *and* the index holds more entries
than the working set. It's a **delta** reselect — a surviving entry never changes slots, so only the
few slots whose occupant actually changed get `fseek`+re-parsed, not the whole set. Skipped entirely
while the waypoint detail screen is open (`ui.screen_waypoint != nullptr`), so a slot can't be recycled
out from under a screen the user is looking at.

**Working-set stability** (the new correctness concern this design introduces): `fixed_waypoint_index`
survives a reselect if its slot's source index-entry is unchanged before/after — reselect never moves a
survivor, so this is an equality check, not a lat/lon re-resolve. `selected_waypoint_index` is cleared
defensively on every reselect. `Waypoint::found` now has `IndexEntry.found` (keyed by PSRAM index
entry, not SRAM slot) as its source of truth, written through via `gpx_loader::markWaypointFound()` so
it survives the slot being recycled later. Three UI-Task write sites
(`navigation.cpp::handleTapAt()`'s selection/found writes, `waypoint_screen.cpp`'s fix/unfix button)
picked up `ui_state_mutex` protection for their `Waypoint` struct writes, since `ui.waypoints[]` is no
longer written only at discrete load events — the System Task's reselect is now a second, ongoing
writer. Render-path *reads* remain unprotected, matching this codebase's pre-existing writer-takes-it/
reader-mostly-doesn't convention (see ADR-0023's Consequences for why that's an accepted, narrow risk
rather than an oversight).

**Measured cost**: +3,640 B static SRAM for the whole feature (`MAX_WAYPOINTS` held at 200 throughout,
isolated from ADR-0022's unrelated cap history via a `readelf -S` diff, not the `pio run` summary
percentage alone). PSRAM: index + selection scratch ≈ 250KB, well under 16% of the 8MB chip.

**Field verification**: cold selection and a forced high-churn reselect (174/200 slots, Sydney-area
synthetic center) both matched an independent Haversine oracle exactly on real hardware; HDOP gating
and the 150m movement threshold's stationary-silence both behaved correctly; a live end-to-end test
writing 50 fresh waypoints near the real GPS position and reloading put them correctly at the top of
the working set. This pass also caught two real bugs — `gpx_loader::init()` was never called anywhere
(the whole feature was silently dead until fixed), and a test-file generator violated the parser's
one-element-per-line assumption. **Still open**: the automatic GPS-driven reselect call site wasn't
observed firing from real (non-injected) movement, and concurrent SD access during a reselect is
unverified (no SD-access mutex exists anywhere in this codebase). Full detail in ADR-0023.

**Code References**:
- Index: `include/gpx/gpx_index.h` / `src/gpx/gpx_index.cpp`
- Selection/reselect: `src/gpx/gpx_loader.cpp` - `selectAndMaterialize()`, `reselect()`, `buildFileIndex()`
- Movement trigger: `src/utils/task_manager.cpp` - `updateStatusLabels()`
- Haversine helper: `include/utils/geo.h` / `src/utils/geo.cpp`
- Stability audit: `src/ui/navigation.cpp` (`handleTapAt()`), `src/ui/waypoint_screen.cpp` (fix/unfix)
- Debug verification commands: `src/utils/diagnostics.cpp` - `gpx index list/reselect/gentest`

---

## Navigation Modes System

**Status**: Complete ✅ | [Complete Guide](docs/navigation_modes.md)

Dual-mode navigation system allowing users to choose between heading-up (walking direction always points "up") and north-up (fixed north orientation).

**Key Architecture**:
- **Heading-Up Mode** (Default) - Radar rotates to match GPS heading, user always moves "forward"
- **North-Up Mode** (Classic) - North fixed at top, traditional map orientation
- **Stationary Handling** - 10-second timeout, then revert to north-up (GPS heading unreliable when stopped)

**User Problem Solved**: "when I turn left my brain was expecting to move forward but in the radar I was moving left" - cognitive dissonance eliminated

**Heading Source** — ⚠️ **this is now the compass, not GPS.** GPS heading fusion was removed from
`navigation.cpp` entirely; the QMC5883L is the sole heading source, read at 10Hz by the System Task
and applied via `ui.current_heading`. GPS still supplies *position*, over **UBX** (NAV-PVT), not NMEA.
The description below is retained only to explain the older design:
- ~~NMEA RMC sentence fields 7-8 (speed + course)~~ — GPS is UBX binary; RMC is not parsed
- ~~Reliability threshold: 0.5 knots minimum speed~~ — no longer applicable, a compass works at rest
- ~~Update rate: 1 Hz~~ — heading updates at 10Hz from the compass

**Coordinate Rotation**:
- 2D rotation matrix: `-heading` radians (counterclockwise)
- Applied in `latLonToScreen()` after Haversine calculation
- All waypoints/indicators rotate around user position

**North Indicator** (Heading-Up Mode Only):
- Red circle (30px) with white "N" text
- Position: 50px from screen edge
- Rotates to always point toward true north
- Shows absolute orientation while radar rotates

**Settings Integration**:
- NVS persistence: `heading_up_mode` (default: true)
- Settings UI: Settings > Display > Navigation Mode dropdown
- Real-time switching (no restart required)

**Performance**: <1ms rotation overhead for 50 waypoints @ 240MHz, +1,848 bytes flash

**Code References**:
- GPS parsing: `src/hardware/sensors/gps_bh880.cpp:37-90` - UBX NAV-PVT course/speed extraction
- Rotation: `src/ui/navigation.cpp:98-118` - `rotatePoint()` function
- North indicator: `src/ui/navigation.cpp:248-286` - `drawNorthIndicator()`
- Update logic: `src/ui/navigation.cpp:528-541` - Three-state heading management
- Settings: `src/ui/settings_screen.cpp:1012-1051` - Navigation mode dropdown

**Industry Standard**: Found in aviation GPS (3 modes), marine chart plotters, automotive navigation (Google Maps, Waze), hiking GPS (Garmin), and professional surveying equipment.

**Reference Documentation**: [`docs/navigation_modes.md`](docs/navigation_modes.md) - Complete user guide and technical details

---

## Beacon Proximity System

**Status**: Complete ✅ | [Complete Guide](docs/beacon_proximity.md)

BLE-based item finder that activates at 50m zoom. Scans for a configured beacon MAC address and provides real-time visual arc gauge + sonar audio feedback.

**Key Architecture**:
- **Visual**: Cyan arc fills clockwise around radar edge (0° = no signal, 355° = full at -45 dBm EMA RSSI)
- **Audio**: Buzzer sonar pulses at 1800ms → 200ms as beacon gets closer
- **Zoom-gated**: Only activates at 50m zoom — stops when zooming out
- **RSSI Processing**: EMA (α=0.4) + zone hysteresis (±3 dBm) + trend detection over 10 samples
- **BLE Stack**: NimBLE (`h2zero/NimBLE-Arduino@^1.4.0`) — ~25KB SRAM (was ~65KB with Bluedroid)

**Zone Thresholds** — the zone decides *whether* to beep and whether to show the solid CLOSE fill.
It no longer decides the tempo:
- OUT_OF_RANGE: < -90 dBm (silent, no ring)
- VERY_FAR / FAR / MEDIUM: -90 / -85 / -75 dBm
- CLOSE: ≥ -65 dBm (solid fill + ball + star)

**Sonar tempo is continuous and linear in dBm** — 1500 ms at -90 dBm → 150 ms at -50 dBm, driven by
`rssi_display` (the slow EMA), **not** `rssi_ema`. A first cut used `rssi_ema` and it beat audibly
unsteady — RSSI wobbles ±3-5 dB standing still, and over this 40 dB span that's a ~25% swing in beat
period. A continuous tempo only glides if the value driving it is itself smooth. Four discrete rates
made a search unnavigable before that: most of it happens inside one zone, where moving produced no
audible change, and someone hunting listens for *change in response to their own movement*, not
absolute level. Linear in dBm is exact, not approximate — RSSI ≈ C − 20·log₁₀(d), so equal dBm steps
are equal distance *ratios*.

**Beep duration encodes trend, continuously** — interpolated from the raw regression slope
(`trend_slope_dbm_s`), saturating at ±2 dBm/s: 30 ms neutral ± up to 30 ms, floored at 12 ms. A first
cut switched on the 3-state `MovementTrend` enum instead, and it was the worst part of the result —
standing still the slope hovers near zero, so the classifier flipped
APPROACHING/STABLE/DEPARTING at random and the beep length jumped 60→30→12 ms beat to beat. The buzzer
has no pitch, but duration was already a free parameter, and "warmer/colder" beats absolute level when
the environment, tag orientation and your own body all shift the absolute.

**Beacon has absolute priority over the waypoint sonar.** When `beacon_proximity::isInRange()` is
true, `updateWaypointFixSonar()` **releases the fixed waypoint** outright — a beacon is a thing to
find, a fixed waypoint is an area you walk into. Merely yielding the buzzer wasn't enough: a fix keeps
every other waypoint hidden and would re-take the sonar as soon as the beacon dipped out of range.

**Settings**: Target MAC, measured power (dBm @ 1m), path loss exponent — all NVS-persistent (`bcn_*` keys)

**Serial Commands**: `beacon status`, `beacon scan`, `beacon test`, `beacon zone`, `beacon trend`

**✅ The 2 Hz rate starvation is fixed and verified on hardware** (§7.3a–d, built 2026-07-31). The feed
was capped at one RSSI sample per 500 ms by three independent things — NimBLE's duplicate filtering, a
`g_pScan->stop()` on first hit, and a 500 ms poll loop — while the tag advertised at 200 ms. It is now
**one continuous passive scan** running forever, duplicates off, results not stored. Measured live on
device: **4.24–4.37 Hz** (mean gap ~230 ms), up from 2.0 Hz — ~89 advertisements/sec delivered across
~30 nearby devices, 69 matching the target MAC in one sample window. **This was never a CPU problem —
240 MHz bought it nothing.**

Everything derived from that feed was re-derived with it, which is the actual lesson: both EMAs are
now **τ-based off measured elapsed time** (0.5 s fast / 2.0 s slow) rather than fixed per-sample α,
zone confirmation is a **duration** (1000 ms) rather than a sample count, and the trend slope is
regressed against **real time** in dBm/s rather than per-cycle. Left alone, every one of those would
have silently changed meaning by 2.5–5× the moment the rate moved.

The two τ constants split **decision vs presentation**, not just "fast vs slow": `rssi_ema` (0.5 s)
feeds zone classification and trend, which have their own hysteresis/confirmation downstream, so
latency hurts more than noise there. `rssi_display` (2.0 s, raised from an initial 1.0 s after the
tempo it drives came out choppy) feeds the ring width and sonar tempo, which are shown/heard **raw** —
there noise is the whole problem, and rhythm error is far more perceptible than visual lag.

**⚠️ The tag must advertise in Legacy (BLE 4.0) mode.** `CONFIG_BT_NIMBLE_EXT_ADV` is not set, so the
firmware only receives legacy advertisements. A tag switched to *Extend Advertisement* or *PHY Coded*
(BLE 5.0) is **completely invisible**, and the symptom is indistinguishable from a dead tag: -127 dBm,
OUT_OF_RANGE, silent. `beacon status` distinguishes them — a large `Scan callbacks: N total` with
`0 matched target` means the scan is healthy and the tag is the problem.

**Passive scanning is load-bearing, not just a power choice.** With active scanning and a legacy
`ADV_IND` advertiser, NimBLE withholds the `onResult` callback until the scan response arrives
(`NimBLEScan.cpp`). Passive short-circuits that and delivers on the advertisement itself.

⚠️ **`setAdvertisedDeviceCallbacks(cb, wantDuplicates)` calls `setDuplicateFilter(!wantDuplicates)`
internally** — calling it after the explicit `setDuplicateFilter(false)` silently puts the feed back
to 2 Hz. `debugScanAll()` did exactly this and is now fixed. See
[`docs/beacon_proximity.md`](docs/beacon_proximity.md).

**Direction finding** ("which way do I walk?") was **blocked on that rate work and is now unblocked** —
at 2 Hz a rotation yielded 1.7 samples per 30° bin (noise); at 5–10 Hz it yields 4–8, which is usable.
Body-shadow DF using the compass; true BT 5.1 AoA is impossible on this hardware (single antenna, no
CTE IQ). Design: [`docs/beacon_direction_finding.md`](docs/beacon_direction_finding.md).

**Code References**:
- Arc drawing: `src/ui/navigation.cpp:390-420` - `drawBeaconProximityGauge()`
- BLE module: `src/hardware/connectivity/beacon_proximity.cpp` - scanning, EMA, zones
- Zoom gating: `src/utils/task_manager.cpp:79-94`
- Serial commands: `src/utils/diagnostics.cpp:1255-1403`
- Settings UI: `src/ui/settings_screen.cpp:1241-1310`

**Reference Documentation**: [`docs/beacon_proximity.md`](docs/beacon_proximity.md)

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

**Complete Documentation Guide**: [`docs/documentation_standards.md`](docs/documentation_standards.md)

---

*This document serves as the master reference for ESP32-S3 Touch LCD projects. Keep it updated as the architecture evolves.*

**Last updated**: 2026-08-05 (two-tier waypoint index added — PSRAM full index + SRAM closest-N working set, `MAX_WAYPOINTS` stays 200, see ADR-0023. Same day, earlier: `MAX_WAYPOINTS` raised 50 → 500 then rolled back to 200 after a boot failure; Haversine replaced with equirectangular approximation in `drawWaypoints()`/`latLonToScreen()`, see ADR-0022. Previously: 2026-07-31, waypoint desc/hint moved to PSRAM, freeing ~64KB SRAM and flash; full-subsystem performance audit: beacon BLE rate, sonar rhythm, direction-finding feasibility)
