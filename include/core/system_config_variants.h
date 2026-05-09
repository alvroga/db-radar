#ifndef SYSTEM_CONFIG_VARIANTS_H
#define SYSTEM_CONFIG_VARIANTS_H

/**
 * @file system_config_variants.h
 * @brief Hardware variant configurations for ESP32-S3 Touch LCD Template
 *
 * This file provides predefined configurations for different hardware setups.
 * To use a variant, simply uncomment the desired variant in system_config.h
 */

// =============================================================================
// HARDWARE VARIANT: Waveshare ESP32-S3-Touch-LCD-2.1 (Default)
// =============================================================================

#ifdef HARDWARE_WAVESHARE_ESP32_S3_TOUCH_LCD_2_1
namespace variant_waveshare_2_1 {
    namespace display {
        constexpr int SCREEN_WIDTH = 480;
        constexpr int SCREEN_HEIGHT = 480;
        constexpr int PCLK_HZ = 10000000;      // 10MHz - proven stable
        constexpr int BUFFER_LINES = 40;

        // RGB timing parameters (critical for stability)
        constexpr int HSYNC_PULSE_WIDTH = 8;
        constexpr int HSYNC_BACK_PORCH = 20;   // Critical: was 16, now 20
        constexpr int HSYNC_FRONT_PORCH = 20;  // Critical: was 16, now 20
        constexpr int VSYNC_PULSE_WIDTH = 4;
        constexpr int VSYNC_BACK_PORCH = 8;
        constexpr int VSYNC_FRONT_PORCH = 10;  // Critical: was 8, now 10
    }

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

        // GPS Module (optional)
        constexpr int GPS_TX = 43;
        constexpr int GPS_RX = 44;
    }

    namespace communication {
        constexpr uint32_t I2C_FREQ_HZ = 400000;     // 400kHz
        constexpr uint32_t SPI_FREQ_HZ = 4000000;    // 4MHz
    }
}
#endif

// =============================================================================
// HARDWARE VARIANT: Custom 320x240 Display
// =============================================================================

#ifdef HARDWARE_CUSTOM_320_240
namespace variant_custom_320_240 {
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

    namespace pins {
        // Status LED
        constexpr int LED = 2;

        // I2C Bus (shared by multiple devices)
        constexpr int I2C_SDA = 21;
        constexpr int I2C_SCL = 22;

        // Display SPI Interface (command interface)
        constexpr int LCD_MOSI = 23;
        constexpr int LCD_CLK = 18;

        // Display RGB Interface (data) - different pins for custom board
        constexpr int LCD_PCLK = 19;
        constexpr int LCD_DE = 5;
        constexpr int LCD_VSYNC = 16;
        constexpr int LCD_HSYNC = 17;

        // RGB Data Pins (16-bit interface) - example different mapping
        constexpr int LCD_DATA_PINS[] = {25,26,27,14,12,13,15,4,0,3,1,33,32,35,34,39};

        // Backlight PWM
        constexpr int LCD_BL = 36;

        // SD Card
        constexpr int SD_CLK = 18;   // Shared with LCD_CLK
        constexpr int SD_CMD = 23;   // Shared with LCD_MOSI
        constexpr int SD_D0 = 37;

        // GPS Module (optional)
        constexpr int GPS_TX = 9;
        constexpr int GPS_RX = 10;
    }

    namespace communication {
        constexpr uint32_t I2C_FREQ_HZ = 100000;     // Lower frequency for custom board
        constexpr uint32_t SPI_FREQ_HZ = 2000000;    // Lower SPI frequency
    }
}
#endif

// =============================================================================
// HARDWARE VARIANT: Custom 240x320 Display (Portrait)
// =============================================================================

#ifdef HARDWARE_CUSTOM_240_320
namespace variant_custom_240_320 {
    namespace display {
        constexpr int SCREEN_WIDTH = 240;
        constexpr int SCREEN_HEIGHT = 320;
        constexpr int PCLK_HZ = 6000000;       // 6MHz for portrait display
        constexpr int BUFFER_LINES = 24;

        // Timing parameters for portrait orientation
        constexpr int HSYNC_PULSE_WIDTH = 3;
        constexpr int HSYNC_BACK_PORCH = 12;
        constexpr int HSYNC_FRONT_PORCH = 12;
        constexpr int VSYNC_PULSE_WIDTH = 2;
        constexpr int VSYNC_BACK_PORCH = 8;
        constexpr int VSYNC_FRONT_PORCH = 10;
    }

    namespace pins {
        // Similar to 320x240 but optimized for portrait orientation
        constexpr int LED = 2;
        constexpr int I2C_SDA = 21;
        constexpr int I2C_SCL = 22;
        constexpr int LCD_MOSI = 23;
        constexpr int LCD_CLK = 18;
        constexpr int LCD_PCLK = 19;
        constexpr int LCD_DE = 5;
        constexpr int LCD_VSYNC = 16;
        constexpr int LCD_HSYNC = 17;
        constexpr int LCD_DATA_PINS[] = {25,26,27,14,12,13,15,4,0,3,1,33,32,35,34,39};
        constexpr int LCD_BL = 36;
        constexpr int SD_CLK = 18;
        constexpr int SD_CMD = 23;
        constexpr int SD_D0 = 37;
        constexpr int GPS_TX = 9;
        constexpr int GPS_RX = 10;
    }

    namespace communication {
        constexpr uint32_t I2C_FREQ_HZ = 100000;
        constexpr uint32_t SPI_FREQ_HZ = 2000000;
    }
}
#endif

// =============================================================================
// VARIANT SELECTOR FUNCTIONS
// =============================================================================

namespace system_config {

// Export the selected variant configuration to system_config namespace
#ifdef HARDWARE_WAVESHARE_ESP32_S3_TOUCH_LCD_2_1
    // Default Waveshare configuration
    namespace display = variant_waveshare_2_1::display;
    namespace pins = variant_waveshare_2_1::pins;
    namespace variant_communication = variant_waveshare_2_1::communication;

    constexpr const char* HARDWARE_NAME = "Waveshare ESP32-S3-Touch-LCD-2.1";
    constexpr const char* HARDWARE_DESCRIPTION = "2.1\" 480x480 IPS LCD with capacitive touch";

#elif defined(HARDWARE_CUSTOM_320_240)
    // Custom 320x240 configuration
    namespace display = variant_custom_320_240::display;
    namespace pins = variant_custom_320_240::pins;
    namespace variant_communication = variant_custom_320_240::communication;

    constexpr const char* HARDWARE_NAME = "Custom 320x240";
    constexpr const char* HARDWARE_DESCRIPTION = "3.2\" 320x240 LCD (landscape)";

#elif defined(HARDWARE_CUSTOM_240_320)
    // Custom 240x320 configuration
    namespace display = variant_custom_240_320::display;
    namespace pins = variant_custom_240_320::pins;
    namespace variant_communication = variant_custom_240_320::communication;

    constexpr const char* HARDWARE_NAME = "Custom 240x320";
    constexpr const char* HARDWARE_DESCRIPTION = "2.4\" 240x320 LCD (portrait)";

#else
    #error "No hardware variant selected! Please define one of: HARDWARE_WAVESHARE_ESP32_S3_TOUCH_LCD_2_1, HARDWARE_CUSTOM_320_240, HARDWARE_CUSTOM_240_320"
#endif
} // namespace system_config

// =============================================================================
// VARIANT VALIDATION
// =============================================================================

// Compile-time validation for each variant
#ifdef HARDWARE_WAVESHARE_ESP32_S3_TOUCH_LCD_2_1
static_assert(system_config::display::SCREEN_WIDTH == 480, "Waveshare variant must have 480px width");
static_assert(system_config::display::SCREEN_HEIGHT == 480, "Waveshare variant must have 480px height");
#endif

#ifdef HARDWARE_CUSTOM_320_240
static_assert(system_config::display::SCREEN_WIDTH == 320, "320x240 variant must have 320px width");
static_assert(system_config::display::SCREEN_HEIGHT == 240, "320x240 variant must have 240px height");
#endif

#ifdef HARDWARE_CUSTOM_240_320
static_assert(system_config::display::SCREEN_WIDTH == 240, "240x320 variant must have 240px width");
static_assert(system_config::display::SCREEN_HEIGHT == 320, "240x320 variant must have 320px height");
#endif

#endif // SYSTEM_CONFIG_VARIANTS_H