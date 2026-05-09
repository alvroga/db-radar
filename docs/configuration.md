# Configuration System Guide

## Overview

The ESP32-S3 Touch LCD Template features a comprehensive configuration management system that consolidates all magic numbers, timing parameters, and hardware-specific settings into a centralized, organized structure. This system enables easy customization for different hardware variants and project requirements.

## Central Configuration Files

### `include/system_config.h`
The main configuration header that consolidates all system parameters into organized namespaces:

- **Hardware Variant Selection**: Choose between supported display configurations
- **Display Configuration**: Screen dimensions, timing parameters, buffer settings
- **Pin Configuration**: GPIO assignments for different hardware variants
- **Communication Settings**: I2C, SPI, and serial communication parameters
- **Timing Configuration**: Intervals for scanning, sensor readings, system monitoring
- **UI Configuration**: Layout dimensions, colors, animation settings
- **Memory Configuration**: Heap thresholds, pool settings, monitoring intervals
- **Feature Enablement**: Runtime flags for optional functionality

### `include/system_config_variants.h`
Hardware variant definitions for different display configurations with complete pin mappings and timing parameters.

## Hardware Variant Support

### Selecting a Hardware Variant

In `include/system_config.h`, uncomment ONE of the following lines:

```cpp
// Hardware variant selection
#define HARDWARE_WAVESHARE_ESP32_S3_TOUCH_LCD_2_1  // Default: 2.1" 480x480
// #define HARDWARE_CUSTOM_320_240                    // Custom: 320x240 display
// #define HARDWARE_CUSTOM_240_320                    // Custom: 240x320 display
```

### Supported Hardware Variants

#### 1. Waveshare ESP32-S3-Touch-LCD-2.1 (Default)
- **Display**: 2.1" 480×480 IPS LCD
- **Touch**: CST820 capacitive touch controller
- **Key Features**: Proven stable timing parameters, comprehensive pin mapping
- **Use Case**: Primary development and production hardware

#### 2. Custom 320×240 Display
- **Display**: 3.2" 320×240 LCD (landscape orientation)
- **Timing**: Lower frequency settings for smaller displays
- **Pin Mapping**: Alternative GPIO assignments for custom boards
- **Use Case**: Cost-optimized or space-constrained applications

#### 3. Custom 240×320 Display
- **Display**: 2.4" 240×320 LCD (portrait orientation)
- **Timing**: Portrait-optimized parameters
- **Pin Mapping**: Similar to 320×240 but optimized for portrait mode
- **Use Case**: Portrait applications, UI designs requiring vertical layout

## Configuration Namespaces

### Display Configuration
```cpp
namespace system_config::display {
    constexpr int SCREEN_WIDTH = 480;
    constexpr int SCREEN_HEIGHT = 480;
    constexpr int PCLK_HZ = 10000000;      // 10MHz - proven stable
    constexpr int BUFFER_LINES = 480;      // Full-frame LVGL buffer (1 flush/frame)

    // RGB timing parameters (critical for stability)
    constexpr int HSYNC_PULSE_WIDTH = 8;
    constexpr int HSYNC_BACK_PORCH = 20;   // Critical: was 16, now 20
    constexpr int HSYNC_FRONT_PORCH = 20;  // Critical: was 16, now 20
    constexpr int VSYNC_PULSE_WIDTH = 4;
    constexpr int VSYNC_BACK_PORCH = 8;
    constexpr int VSYNC_FRONT_PORCH = 10;  // Critical: was 8, now 10
}
```

### Pin Configuration
```cpp
namespace system_config::pins {
    // Status LED
    constexpr int LED = 0;

    // I2C Bus (shared by multiple devices)
    constexpr int I2C_SDA = 15;
    constexpr int I2C_SCL = 7;

    // Display RGB Interface (data)
    constexpr int LCD_PCLK = 41;
    constexpr int LCD_DE = 40;
    constexpr int LCD_VSYNC = 39;
    constexpr int LCD_HSYNC = 38;

    // RGB Data Pins (16-bit interface)
    constexpr int LCD_DATA_PINS[] = {5,45,48,47,21,14,13,12,11,10,9,46,3,8,18,17};

    // Backlight PWM
    constexpr int LCD_BL = 6;

    // SD Card (shared with LCD SPI)
    constexpr int SD_CLK = 2;   // Shared with LCD_CLK
    constexpr int SD_CMD = 1;   // Shared with LCD_MOSI
    constexpr int SD_D0 = 42;
}
```

### Timing Configuration
```cpp
namespace system_config::timing {
    // Scanner intervals
    constexpr uint32_t WIFI_SCAN_INTERVAL_MS = 15000;    // WiFi scan every 15 seconds
    constexpr uint32_t BLE_SCAN_INTERVAL_MS = 10000;     // BLE scan every 10 seconds
    constexpr uint32_t BLE_SCAN_DURATION_MS = 3000;      // 3 second active BLE scan

    // Sensor reading intervals
    constexpr uint32_t RTC_READ_INTERVAL_MS = 5000;      // RTC read every 5 seconds
    constexpr uint32_t IMU_UPDATE_INTERVAL_MS = 50;      // IMU update every 50ms (20Hz)
    constexpr uint32_t TOUCH_POLL_INTERVAL_MS = 20;      // Touch poll every 20ms (50Hz)

    // System monitoring
    constexpr uint32_t STATUS_UPDATE_INTERVAL_MS = 1000; // Status update every second
    constexpr uint32_t MEMORY_CHECK_INTERVAL_MS = 60000; // Memory check every minute
    constexpr uint32_t HEALTH_CHECK_INTERVAL_MS = 30000; // Health check every 30 seconds
}
```

### Feature Enablement
```cpp
namespace system_config::features {
    // Hardware features
    constexpr bool ENABLE_WIFI_SCANNING = true;
    constexpr bool ENABLE_BLE_SCANNING = true;
    constexpr bool ENABLE_GPS = false;               // Optional GPS module
    constexpr bool ENABLE_SD_CARD = true;
    constexpr bool ENABLE_IMU = true;
    constexpr bool ENABLE_RTC = true;

    // Software features
    constexpr bool ENABLE_DIAGNOSTICS = true;
    constexpr bool ENABLE_MEMORY_MONITORING = true;
    constexpr bool ENABLE_PERFORMANCE_MONITORING = true;
    constexpr bool ENABLE_SERIAL_COMMANDS = true;

    // Debug features
    constexpr bool ENABLE_DEBUG_LOGGING = true;
    constexpr bool ENABLE_PERFORMANCE_LOGGING = false;
    constexpr bool ENABLE_MEMORY_LOGGING = false;
    constexpr bool ENABLE_I2C_LOGGING = false;
}
```

## Runtime Configuration Commands

The template provides comprehensive serial commands for viewing and understanding the current configuration:

### Available Commands

```bash
# Show all configuration values
config show

# Display-specific parameters
config display

# Timing intervals and delays
config timing

# GPIO pin assignments
config pins

# Future: Runtime parameter changes
config set <parameter> <value>
```

### Example: Viewing Current Configuration

```
> config show
[CONFIG] Hardware Configuration:
  Variant: Waveshare ESP32-S3-Touch-LCD-2.1
  Description: 2.1" 480x480 IPS LCD with capacitive touch

[CONFIG] Display: 480x480 @ 10000000Hz PCLK
[CONFIG] Communication: I2C=400000Hz, SPI=4000000Hz
[CONFIG] Features: WiFi=ON, BLE=ON, GPS=OFF, SD=ON, IMU=ON, RTC=ON
```

### Example: Display Configuration Details

```
> config display
[CONFIG] Display Configuration:
  Screen: 480x480 pixels
  Pixel Clock: 10000000 Hz (10 MHz)
  Buffer Lines: 40

  RGB Timing:
    HSYNC: pulse=8, back=20, front=20
    VSYNC: pulse=4, back=8, front=10

  Critical timing parameters optimized for jitter-free operation
```

## Customizing Configuration

### For New Hardware Variants

1. **Create new variant section** in `system_config_variants.h`:
```cpp
#ifdef HARDWARE_MY_CUSTOM_BOARD
namespace variant_my_custom {
    namespace display {
        constexpr int SCREEN_WIDTH = 320;
        constexpr int SCREEN_HEIGHT = 240;
        // ... other parameters
    }

    namespace pins {
        constexpr int I2C_SDA = 21;
        constexpr int I2C_SCL = 22;
        // ... pin assignments
    }
}
#endif
```

2. **Add variant selection** in `system_config.h`:
```cpp
// #define HARDWARE_MY_CUSTOM_BOARD     // My custom board variant
```

3. **Add variant export** in `system_config_variants.h`:
```cpp
#elif defined(HARDWARE_MY_CUSTOM_BOARD)
    namespace display = variant_my_custom::display;
    namespace pins = variant_my_custom::pins;
    // ...
    constexpr const char* HARDWARE_NAME = "My Custom Board";
#endif
```

### For Project-Specific Modifications

1. **Timing Adjustments**: Modify values in the appropriate namespace
2. **Feature Toggles**: Change boolean flags in `features` namespace
3. **Pin Remapping**: Update pin assignments in `pins` namespace
4. **Memory Tuning**: Adjust thresholds in `memory` namespace

### Best Practices

- **Always recompile** after configuration changes (values are compile-time constants)
- **Test thoroughly** after hardware variant changes
- **Use static assertions** to validate configuration consistency
- **Document custom variants** for team members
- **Version control** configuration changes with clear commit messages

## Compile-Time Validation

The system includes automatic validation to catch configuration errors:

```cpp
// Example validations
static_assert(display::SCREEN_WIDTH > 0, "Screen width must be positive");
static_assert(display::SCREEN_HEIGHT > 0, "Screen height must be positive");
static_assert(display::PCLK_HZ > 1000000, "Pixel clock must be at least 1MHz");
static_assert(ui::HEADER_HEIGHT < display::SCREEN_HEIGHT, "Header height too large");
```

## Future Enhancements

- **Runtime Parameter Changes**: Save configuration to SD card for persistent changes
- **Web Interface**: Configure parameters via web interface
- **Configuration Profiles**: Switch between predefined configuration sets
- **Auto-Detection**: Automatic hardware variant detection where possible

## Migration from Legacy Code

When adapting existing code to use the central configuration system:

1. **Replace magic numbers** with namespace references:
```cpp
// Before
if (millis() - last_scan >= 15000) {

// After
if (millis() - last_scan >= system_config::timing::WIFI_SCAN_INTERVAL_MS) {
```

2. **Update module Config structs** to use central defaults:
```cpp
struct Config {
    int screen_width = system_config::display::SCREEN_WIDTH;
    int screen_height = system_config::display::SCREEN_HEIGHT;
    // ...
};
```

3. **Use feature flags** instead of conditional compilation:
```cpp
// Before
#ifdef ENABLE_DEBUG
    Serial.println("Debug message");
#endif

// After
if (system_config::features::ENABLE_DEBUG_LOGGING) {
    Serial.println("Debug message");
}
```

This configuration system ensures the template remains flexible, maintainable, and easily adaptable to different hardware configurations and project requirements.