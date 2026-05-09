#ifndef SYSTEM_CONFIG_H
#define SYSTEM_CONFIG_H

#include <stdint.h>

/**
 * @file system_config.h
 * @brief Central configuration header for ESP32-S3 Touch LCD Template
 *
 * This file consolidates all configuration parameters, magic numbers, and
 * hardware-specific settings to make the template easily customizable.
 */

namespace system_config {

// =============================================================================
// HARDWARE VARIANT SELECTION
// =============================================================================

// Uncomment ONE of the following to select your hardware configuration:
#define HARDWARE_WAVESHARE_ESP32_S3_TOUCH_LCD_2_1  // Default: 2.1" 480x480
// #define HARDWARE_CUSTOM_320_240                    // Custom: 320x240 display
// #define HARDWARE_CUSTOM_240_320                    // Custom: 240x320 display

// =============================================================================
// DISPLAY CONFIGURATION
// =============================================================================

#ifdef HARDWARE_WAVESHARE_ESP32_S3_TOUCH_LCD_2_1
namespace display {
    constexpr int SCREEN_WIDTH = 480;
    constexpr int SCREEN_HEIGHT = 480;
    constexpr int PCLK_HZ = 10000000;      // 10MHz - proven stable
    constexpr int BUFFER_LINES = 480;      // Full-screen buffer - eliminates visible wipe during screen transitions (rotation 10KB temp buffer fits in 64KB LV_MEM_SIZE)
    constexpr int BOUNCE_BUFFER_LINES = 10; // SRAM bounce buffer lines (IDF 5.5): 2×(10×480×2)=18.75KB SRAM, eliminates PSRAM/DMA bus contention

    // Display rotation to compensate for 90° CCW physical rotation in enclosure
    constexpr int ROTATION_DEGREES = 90;   // 90° CW software rotation

    // RGB timing parameters (critical for stability)
    constexpr int HSYNC_PULSE_WIDTH = 8;
    constexpr int HSYNC_BACK_PORCH = 20;   // Critical: was 16, now 20
    constexpr int HSYNC_FRONT_PORCH = 20;  // Critical: was 16, now 20
    constexpr int VSYNC_PULSE_WIDTH = 4;
    constexpr int VSYNC_BACK_PORCH = 8;
    constexpr int VSYNC_FRONT_PORCH = 10;  // Critical: was 8, now 10
}
#endif

#ifdef HARDWARE_CUSTOM_320_240
namespace display {
    constexpr int SCREEN_WIDTH = 320;
    constexpr int SCREEN_HEIGHT = 240;
    constexpr int PCLK_HZ = 8000000;       // 8MHz for smaller displays
    constexpr int BUFFER_LINES = 30;

    // Standard timing parameters
    constexpr int HSYNC_PULSE_WIDTH = 4;
    constexpr int HSYNC_BACK_PORCH = 16;
    constexpr int HSYNC_FRONT_PORCH = 16;
    constexpr int VSYNC_PULSE_WIDTH = 2;
    constexpr int VSYNC_BACK_PORCH = 6;
    constexpr int VSYNC_FRONT_PORCH = 8;
}
#endif

// =============================================================================
// PIN CONFIGURATION
// =============================================================================

#ifdef HARDWARE_WAVESHARE_ESP32_S3_TOUCH_LCD_2_1
namespace pins {
    // Status LED
    constexpr int LED = 0;

    // I2C Bus (shared by multiple devices)
    constexpr int I2C_SDA = 15;
    constexpr int I2C_SCL = 7;

    // Display SPI Interface (command interface)
    constexpr int LCD_MOSI = 1;
    constexpr int LCD_CLK = 2;

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

    // GPS Module (LC76G UART)
    constexpr int GPS_TX = 43;  // ESP32 TX -> GPS RX
    constexpr int GPS_RX = 44;  // ESP32 RX -> GPS TX
    constexpr uint32_t GPS_BAUD = 115200;
}
#endif

// Hardware variant identification
#ifdef HARDWARE_WAVESHARE_ESP32_S3_TOUCH_LCD_2_1
constexpr const char* HARDWARE_NAME = "Waveshare ESP32-S3-Touch-LCD-2.1";
constexpr const char* HARDWARE_DESCRIPTION = "2.1\" 480x480 IPS LCD with capacitive touch";
#elif defined(HARDWARE_CUSTOM_320_240)
constexpr const char* HARDWARE_NAME = "Custom 320x240";
constexpr const char* HARDWARE_DESCRIPTION = "3.2\" 320x240 LCD (landscape)";
#elif defined(HARDWARE_CUSTOM_240_320)
constexpr const char* HARDWARE_NAME = "Custom 240x320";
constexpr const char* HARDWARE_DESCRIPTION = "2.4\" 240x320 LCD (portrait)";
#else
constexpr const char* HARDWARE_NAME = "Unknown";
constexpr const char* HARDWARE_DESCRIPTION = "Hardware variant not selected";
#endif

// =============================================================================
// COMMUNICATION SETTINGS
// =============================================================================

namespace communication {
    // I2C Configuration
    constexpr uint32_t I2C_FREQ_HZ = 400000;     // 400kHz (can reduce to 100kHz if errors)
    constexpr int I2C_TIMEOUT_MS = 100;
    constexpr int I2C_RETRY_COUNT = 3;
    constexpr int I2C_RETRY_DELAY_MS = 10;

    // SPI Configuration
    constexpr uint32_t SPI_FREQ_HZ = 4000000;     // 4MHz for command interface
    constexpr int SPI_TIMEOUT_MS = 100;

    // Serial Configuration
    constexpr uint32_t SERIAL_BAUD = 115200;
    constexpr int SERIAL_TIMEOUT_MS = 1000;
}

// =============================================================================
// TIMING CONFIGURATION
// =============================================================================

namespace timing {
    // Scanner intervals
    constexpr uint32_t WIFI_SCAN_INTERVAL_MS = 15000;    // WiFi scan every 15 seconds
    constexpr uint32_t BLE_SCAN_INTERVAL_MS = 10000;     // BLE scan every 10 seconds
    constexpr uint32_t BLE_SCAN_DURATION_MS = 3000;      // 3 second active BLE scan

    // Sensor reading intervals
    constexpr uint32_t RTC_READ_INTERVAL_MS = 5000;      // RTC read every 5 seconds
    constexpr uint32_t TOUCH_POLL_INTERVAL_MS = 20;      // Touch poll every 20ms (50Hz)
    constexpr uint32_t GPS_UPDATE_INTERVAL_MS = 1000;    // GPS read every 1 second

    // System monitoring
    constexpr uint32_t STATUS_UPDATE_INTERVAL_MS = 1000; // Status update every second
    constexpr uint32_t MEMORY_CHECK_INTERVAL_MS = 60000; // Memory check every minute
    constexpr uint32_t HEALTH_CHECK_INTERVAL_MS = 30000; // Health check every 30 seconds

    // Display and UI
    constexpr uint32_t LVGL_TICK_PERIOD_MS = 5;          // LVGL tick every 5ms
    constexpr uint32_t SCREEN_TIMEOUT_MS = 300000;       // Screen timeout 5 minutes
    constexpr uint32_t ANIMATION_DURATION_MS = 200;      // UI animation duration

    // Initialization delays
    constexpr uint32_t POWER_ON_DELAY_MS = 100;          // Power stabilization
    constexpr uint32_t LCD_RESET_DELAY_MS = 10;          // LCD reset duration
    constexpr uint32_t I2C_SETUP_DELAY_MS = 50;          // I2C bus stabilization
}

// =============================================================================
// UI CONFIGURATION
// =============================================================================

namespace ui {
    // Layout dimensions (relative to display size)
    constexpr int HEADER_HEIGHT = 60;
    constexpr int SAFE_MARGIN = 16;
    constexpr int BUTTON_HEIGHT = 50;
    constexpr int BUTTON_SPACING = 10;

    // Content area calculations
    constexpr int CONTENT_WIDTH = display::SCREEN_WIDTH;
    constexpr int CONTENT_HEIGHT = display::SCREEN_HEIGHT - HEADER_HEIGHT;

    // Colors (RGB565)
    constexpr uint16_t COLOR_PRIMARY = 0x0080;      // Blue
    constexpr uint16_t COLOR_SECONDARY = 0xF800;    // Red
    constexpr uint16_t COLOR_SUCCESS = 0x07E0;      // Green
    constexpr uint16_t COLOR_WARNING = 0xFFE0;      // Yellow
    constexpr uint16_t COLOR_BACKGROUND = 0x0000;   // Black
    constexpr uint16_t COLOR_TEXT = 0xFFFF;         // White

    // Animation settings
    constexpr uint32_t FADE_DURATION_MS = timing::ANIMATION_DURATION_MS;
    constexpr uint32_t SLIDE_DURATION_MS = timing::ANIMATION_DURATION_MS;
}

// =============================================================================
// BACKLIGHT CONFIGURATION
// =============================================================================

namespace backlight {
    constexpr int PWM_FREQ_HZ = 20000;              // 20kHz PWM frequency
    constexpr int PWM_RESOLUTION_BITS = 8;          // 8-bit resolution (0-255)
    constexpr int INIT_BRIGHTNESS_PERCENT = 78;     // Initial brightness 78%
    constexpr int MIN_BRIGHTNESS_PERCENT = 5;       // Minimum brightness 5%
    constexpr int MAX_BRIGHTNESS_PERCENT = 100;     // Maximum brightness 100%
    constexpr int BRIGHTNESS_STEP = 5;              // Brightness adjustment step
}

// =============================================================================
// MEMORY CONFIGURATION
// =============================================================================

namespace memory {
    // Heap monitoring thresholds
    constexpr uint32_t LOW_HEAP_THRESHOLD_BYTES = 10240;     // 10KB warning threshold
    constexpr uint32_t CRITICAL_HEAP_THRESHOLD_BYTES = 5120; // 5KB critical threshold
    constexpr uint32_t LOW_PSRAM_THRESHOLD_BYTES = 40960;    // 40KB PSRAM warning

    // Memory pools disabled — caused boot loops at any meaningful size
    constexpr bool ENABLE_MEMORY_POOLS = false;

    // Memory monitoring intervals
    constexpr uint32_t STATS_UPDATE_INTERVAL_MS = timing::MEMORY_CHECK_INTERVAL_MS;
    constexpr uint32_t INTEGRITY_CHECK_INTERVAL_MS = timing::HEALTH_CHECK_INTERVAL_MS;
}

// =============================================================================
// I2C DEVICE ADDRESSES
// =============================================================================

namespace i2c_addresses {
    constexpr uint8_t TOUCH_CONTROLLER_CST820 = 0x15;   // Touch controller
    constexpr uint8_t RTC_PCF85063 = 0x51;              // Real-time clock
    constexpr uint8_t IO_EXPANDER_TCA9554 = 0x20;       // IO expander
    constexpr uint8_t IMU_QMI8658_LOW = 0x6A;           // IMU (low address)
    constexpr uint8_t IMU_QMI8658_HIGH = 0x6B;          // IMU (high address)
}

// =============================================================================
// FEATURE ENABLEMENT
// =============================================================================

namespace features {
    // ==========================================================================
    // GPS PROTECTION MODE - TIER 1 PRIORITY
    // ==========================================================================
    // GPS is THE CORE FUNCTION of this radar device
    // When GPS_PROTECTION_MODE = true:
    //   - IMU sampling is disabled (I2C interference)
    //   - GPS time sync is disabled (I2C RTC writes interfere with GPS UART)
    //   - I2C operations are throttled to minimum necessary
    //   - System Task priority remains low (GPS uses hardware UART with DMA)
    constexpr bool GPS_PROTECTION_MODE = true;   // Enforce strict GPS priority
    constexpr bool ENABLE_GPS_TIME_SYNC = false; // PERMANENTLY DISABLED (I2C interference)

    // Hardware features
    constexpr bool ENABLE_WIFI_SCANNING = false;     // Disabled for GPS radar project
    constexpr bool ENABLE_BLE_SCANNING = false;      // Disabled for GPS radar project
    constexpr bool ENABLE_GPS = true;                // *** GPS ENABLED FOR RADAR (TIER 1) ***
    constexpr bool ENABLE_SD_CARD = true;
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

// =============================================================================
// VALIDATION FUNCTIONS
// =============================================================================

// Compile-time validation
static_assert(display::SCREEN_WIDTH > 0, "Screen width must be positive");
static_assert(display::SCREEN_HEIGHT > 0, "Screen height must be positive");
static_assert(display::PCLK_HZ > 1000000, "Pixel clock must be at least 1MHz");
static_assert(ui::HEADER_HEIGHT < display::SCREEN_HEIGHT, "Header height too large");
static_assert(timing::LVGL_TICK_PERIOD_MS > 0, "LVGL tick period must be positive");

} // namespace system_config

#endif // SYSTEM_CONFIG_H