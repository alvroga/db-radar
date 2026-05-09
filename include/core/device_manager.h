#ifndef DEVICE_MANAGER_H
#define DEVICE_MANAGER_H

#include "core/arduino_compat.h"
#include "driver/spi_master.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "sdmmc_cmd.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "i2c_manager.h"
#include "gps_bh880.h"
#include "compass_qmc5883l.h"
#include "system_config.h"

namespace device_manager {

// Device configuration - uses central system configuration with override capability
struct Config {
    // Pin definitions (from system_config)
    int led_pin = system_config::pins::LED;
    int lcd_bl = system_config::pins::LCD_BL;
    int i2c_sda = system_config::pins::I2C_SDA;
    int i2c_scl = system_config::pins::I2C_SCL;
    int lcd_mosi_pin = system_config::pins::LCD_MOSI;
    int lcd_clk_pin = system_config::pins::LCD_CLK;
    int lcd_pclk = system_config::pins::LCD_PCLK;
    int lcd_de = system_config::pins::LCD_DE;
    int lcd_vsync = system_config::pins::LCD_VSYNC;
    int lcd_hsync = system_config::pins::LCD_HSYNC;

    // Display timing (from system_config)
    int screen_width = system_config::display::SCREEN_WIDTH;
    int screen_height = system_config::display::SCREEN_HEIGHT;
    int pclk_hz = system_config::display::PCLK_HZ;

    // I2C settings (from system_config)
    uint32_t i2c_freq = system_config::communication::I2C_FREQ_HZ;

    // Backlight settings (from system_config)
    int backlight_freq = system_config::backlight::PWM_FREQ_HZ;
    int backlight_init_percent = system_config::backlight::INIT_BRIGHTNESS_PERCENT;

    // Buffer settings (from system_config)
    int buffer_lines = system_config::display::BUFFER_LINES;
};

// Device state information
struct DeviceState {
    bool led_ok = false;
    bool i2c_ok = false;
    bool exio_ok = false;
    bool rtc_ok = false;
    bool scanner_ok = false;
    bool gps_ok = false;
    bool compass_ok = false;
    bool lcd_ok = false;
    bool backlight_ok = false;
    bool sd_ok = false;
    bool touch_ok = false;
    bool lvgl_ok = false;
    bool button_ok = false;

    // SD card info
    uint32_t sd_mb = 0;
    sdmmc_card_t* sdmmc_card = nullptr;

    // GPS data
    GPSData last_gps_data;

    // Compass data
    CompassData last_compass_data;

    // Hardware handles
    spi_device_handle_t spi_handle = nullptr;
    esp_lcd_panel_handle_t panel_handle = nullptr;
    i2c_manager::exio::State* exio_state = nullptr;
};

// Initialize all hardware devices in correct order
bool initializeAll(const Config& config = Config{});

// Get current device state
const DeviceState& getDeviceState();

// Individual initialization functions
bool initLED(int pin);
bool initI2C(int sda, int scl, uint32_t freq);
bool initEXIO(i2c_manager::exio::State& state);
bool initRTC();
bool initScanner();
bool initGPS();
bool initCompass();
bool initLCD(const Config& config);
bool initSD(const Config& config);
bool initBacklight(const Config& config);
bool initLVGL(const Config& config);
bool initTouch();
bool initButton();

// Status checking functions
void updateSDStatus();
void updateButton();  // Poll button state (call frequently)
void logDeviceStatus();

// LVGL-related functions
void setupLVGLBuffers(int screen_w, int screen_h, int buf_lines);
void enableLVGLProcessing();
bool isLVGLUnlocked();

// VSYNC semaphore — binary semaphore given by on_vsync_cb ISR every ~26.6ms.
// uiTask takes this to gate lv_timer_handler() to frame boundaries.
// Returns nullptr before initLCD() completes.
SemaphoreHandle_t getVsyncSemaphore();

} // namespace device_manager

#endif // DEVICE_MANAGER_H