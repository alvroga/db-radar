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

/**
 * @brief Which core the RGB panel's vsync ISR is actually running on.
 *
 * Diagnostic for backlog §1.5: esp_intr_alloc() binds an interrupt to whichever
 * core calls it, and the panel is created from the boot path, which sdkconfig
 * pins to Core 1 — the same core as uiTask. Recorded by the ISR rather than
 * printed from it (printf is not ISR-safe) and reported by `perf`.
 *
 * @return 0 or 1, or -1 if no vsync has fired yet.
 */
int getVsyncIsrCore();

/**
 * @brief How the 90° display rotation is performed.
 *
 * The panel is mounted 90° CCW (GPS module placement — see ROADMAP), so the image
 * must be rotated. LVGL's own in-place transpose measured 162ms/frame; TILED
 * replaces it with a cache-friendly blocked transpose in flush_cb.
 *
 * NOTE on touch: LVGL's touch transform (lv_indev.c:346) keys off `rotated` and
 * assumes the pixels were rotated. LVGL_SW and TILED both satisfy that, so touch
 * is correct in either. NONE does not — it exists only for A/B measurement, and
 * touch is misaligned by 90° while it is active.
 */
enum class RotMode : uint8_t {
    LVGL_SW = 0,   // LVGL does it (disp_drv.sw_rotate = 1) — the baseline
    NONE    = 1,   // Nobody does it — sideways, measurement only
    TILED   = 2,   // Blocked transpose in flush_cb
};

/**
 * @brief Request a rotation mode change at runtime (thread-safe, any task).
 *
 * Only records the request; the UI Task applies it via applyPendingRotMode() while
 * holding display_mutex, so it never lands mid-refresh.
 */
void requestRotMode(RotMode mode);

/**
 * @brief Apply a pending requestRotMode(), if any. UI TASK ONLY, under display_mutex.
 * @return true if a change was applied this call.
 */
bool applyPendingRotMode();

RotMode     getRotMode();
const char* rotModeName(RotMode m);

/** @brief False if the staging buffers could not be allocated (TILED unavailable). */
bool isTiledRotateAvailable();

} // namespace device_manager

#endif // DEVICE_MANAGER_H