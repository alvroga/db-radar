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

**Central Configuration** (`include/system_config.h`):
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

**Task Architecture** (`src/task_manager.cpp` and `include/task_manager.h`):
- **UI Task** (Core 1, Priority 3) - LVGL processing and touch input for maximum responsiveness
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
    static constexpr UBaseType_t UI_PRIORITY = 3;      // Highest for responsiveness
    static constexpr BaseType_t UI_CORE = 1;           // Core 1 for UI
    static constexpr BaseType_t OTHER_CORE = 0;        // Core 0 for everything else
};
```

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

### **✅ OPTIMIZED DISPLAY PERFORMANCE** (Priority 3.6 Complete)

The project now features **enhanced display performance** through careful optimization while maintaining the rock-solid stability of the proven 10MHz PCLK timing. Comprehensive testing revealed critical stability boundaries that guided the final optimization approach.

**Display Configuration Optimizations** (`include/system_config.h` and `src/device_manager.cpp`):
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
```ini
[env:cc-moat-port]
platform = espressif32
board = esp32-s3-devkitc-1
framework = arduino
board_upload.flash_size = 16MB
board_build.partitions = partitions/partitions.csv
board_build.filesystem = fatfs
board_build.arduino.memory_type = qio_opi
build_flags =
    -DARDUINO_USB_MODE=1
    -DARDUINO_USB_CDC_ON_BOOT=1
    -DLV_CONF_INCLUDE_SIMPLE
    -Iinclude
```

### **Memory Layout**
- **Custom Partitions**: `partitions/partitions.csv` with 3MB app space and 10MB FFat filesystem
- **PSRAM**: QIO OPI for enhanced performance with 64-byte alignment
- **Flash**: 16MB with optimized partition scheme

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
│   └── gyro_qmi8658.cpp      # Raw vendor driver (unused, hardware on board)
├── i2c infrastructure/       # ✅ PRIORITY 1.2 COMPLETE
│   ├── i2c_manager.cpp       # Unified I2C operations with retry logic
│   └── I2C_Driver.cpp        # Legacy compatibility layer
└── vendor/
    └── Gyro_QMI8658.cpp      # Raw vendor driver (needs C++ refactor)
```

### **LVGL Integration**
- **Version**: 8.3.11
- **Configuration**: `LV_CONF_INCLUDE_SIMPLE` mode
- **Memory**: Direct framebuffer access with dual buffers
- **Performance**: 40-line bounce buffer, 64-byte PSRAM alignment

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

### **✅ Display Performance**
```cpp
// Use full refresh for stability
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

**Performance**: O(n) complexity, <2ms for 50 waypoints @ 240MHz, 128 bytes stack allocation

**Code References**:
- Algorithm: `src/ui/navigation.cpp:291-395` - `drawWaypoints()` function
- Config: `include/ui/ui_manager.h:54-81` - `RadarConfig::DISTANCE_FILTER_MULTIPLIER = 10.0f`
- Off-screen drawing: `src/ui/navigation.cpp:250-289` - `drawOffScreenIndicator()`

**Reference Documentation**: [`docs/waypoint_filtering.md`](docs/waypoint_filtering.md) - Complete technical deep-dive

---

## Navigation Modes System

**Status**: Complete ✅ | [Complete Guide](docs/navigation_modes.md)

Dual-mode navigation system allowing users to choose between heading-up (walking direction always points "up") and north-up (fixed north orientation).

**Key Architecture**:
- **Heading-Up Mode** (Default) - Radar rotates to match GPS heading, user always moves "forward"
- **North-Up Mode** (Classic) - North fixed at top, traditional map orientation
- **Stationary Handling** - 10-second timeout, then revert to north-up (GPS heading unreliable when stopped)

**User Problem Solved**: "when I turn left my brain was expecting to move forward but in the radar I was moving left" - cognitive dissonance eliminated

**GPS Heading Source**:
- NMEA RMC sentence fields 7-8 (speed + course)
- Reliability threshold: 0.5 knots minimum speed
- Update rate: 1 Hz (GPS limitation)

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

**Zone Thresholds**:
- OUT_OF_RANGE: < -85 dBm (silent)
- FAR: -85 to -75 dBm (1800ms beep)
- MEDIUM: -75 to -65 dBm (900ms beep)
- CLOSE: ≥ -65 dBm (500ms → 200ms beep)

**Settings**: Target MAC, measured power (dBm @ 1m), path loss exponent — all NVS-persistent (`bcn_*` keys)

**Serial Commands**: `beacon status`, `beacon scan`, `beacon test`, `beacon zone`, `beacon trend`

**Code References**:
- Arc drawing: `src/ui/navigation.cpp:390-420` - `drawBeaconProximityGauge()`
- BLE module: `src/hardware/connectivity/beacon_proximity.cpp` - scanning, EMA, zones
- Zoom gating: `src/utils/task_manager.cpp:79-94`
- Serial commands: `src/utils/diagnostics.cpp:1255-1403`
- Settings UI: `src/ui/settings_screen.cpp:1241-1310`

**Reference Documentation**: [`docs/beacon_proximity.md`](docs/beacon_proximity.md)

---

## Documentation Standards

**IMPORTANT**: This project prioritizes thorough documentation. Every significant implementation must be documented properly.

**Key Principles**:
- **Major features** require CHANGELOG.md entry + component doc (`docs/*.md`)
- **Critical fixes** documented with root cause, solution, and impact
- **Build impact** always measured (flash/RAM changes)
- **Code references** include file:line citations
- **Templates** ensure consistency (CHANGELOG, component docs)

**Always Document**:
- New features or subsystems
- Critical bug fixes
- Architectural changes
- Hardware integration
- User-facing improvements
- Performance optimizations
- Build system changes

**Documentation Flow**:
1. **During**: Add one-line to CHANGELOG.md immediately
2. **After**: Expand with technical details, build impact, user benefits
3. **Component docs**: Create `docs/*.md` for major features
4. **CLAUDE.md**: Update if architecture changed (brief summary + link to docs)
5. **README.md**: Update features list if user-visible

**Quick Checklist** (after significant work):
- [ ] CHANGELOG.md entry
- [ ] Build impact measured
- [ ] Component doc created/updated (if needed)
- [ ] CLAUDE.md updated (if architecture changed)
- [ ] README.md updated (if user-visible)

**Complete Documentation Guide**: [`docs/documentation_standards.md`](docs/documentation_standards.md)

---

*This document serves as the master reference for ESP32-S3 Touch LCD projects. Keep it updated as the architecture evolves.*

**Last updated**: 2026-03-20 (NimBLE migration, logger heap, LVGL 480-line buffers, BH-880/compass/WMM)
