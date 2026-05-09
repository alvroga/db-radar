#include "device_manager.h"
#include "system_config.h"
#include "lcd_st7701.h"
#include "cst820.h"
#include "backlight.h"
#include "rtc_pcf85063.h"
#include "gps_bh880.h"
#include "compass_qmc5883l.h"
#include "navigation/gps_quality.h"
#include "scanner.h"
#include "wifi_manager.h"
#include "settings_manager.h"
#include "diagnostics.h"
#include "navigation.h"
#include "i2c_manager.h"
#include "memory_manager.h"
#include "hardware/input/button.h"
#include "hardware/buzzer.h"
#include "standby_manager.h"
#include "task_manager.h"
#include "ui/ui_manager.h"
#include "system_logger.h"
#include "esp_heap_caps.h"
#include <time.h>
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"
#include <lvgl.h>

namespace device_manager {

// Global state
static Config g_config;
static DeviceState g_device_state;
static volatile bool g_lvgl_unlocked = false;

// LVGL buffers
static lv_color_t *lv_buf1 = nullptr, *lv_buf2 = nullptr;
static lv_disp_draw_buf_t draw_buf;
static lv_disp_drv_t disp_drv;
static lv_indev_drv_t indev_drv;

// Global pointer for DMA callback synchronization
static lv_disp_drv_t* g_current_disp_drv = nullptr;

// VSYNC semaphore — given by on_vsync_cb ISR every ~26.6ms (37.6 Hz).
// uiTask takes it to gate lv_timer_handler() to frame boundaries (tear-free).
static SemaphoreHandle_t g_vsync_sem = nullptr;

// Display timing pin array
static const int DATA_PINS[16] = { 5,45,48,47,21, 14,13,12,11,10,9, 46,3,8,18,17 };

// Forward declarations
static void lvgl_flush_cb(lv_disp_drv_t* drv, const lv_area_t* area, lv_color_t* color_p);
static void lvgl_touch_read_cb(lv_indev_drv_t*, lv_indev_data_t* data);
static inline int16_t scale_to_480(uint16_t raw);
static bool on_color_trans_done(esp_lcd_panel_handle_t panel, const esp_lcd_rgb_panel_event_data_t* edata, void* user_ctx);
static bool on_vsync_cb(esp_lcd_panel_handle_t panel, const esp_lcd_rgb_panel_event_data_t* edata, void* user_ctx);

const DeviceState& getDeviceState() {
    return g_device_state;
}

bool initializeAll(const Config& config) {
    g_config = config;
    Serial.println("==== Device Manager: Initializing Hardware ====");

    // 0. Memory Manager - Phase 2: Ultra-conservative pool enablement (2 blocks each)
    memory_manager::Config mem_config;
    mem_config.enable_tracking = true; // ENABLED: Ultra-conservative memory pools (2+2 blocks)
    mem_config.enable_periodic_check = true; // Enable periodic integrity checks
    mem_config.check_interval_ms = 30000; // Check every 30 seconds
    mem_config.log_stats = false; // Avoid spam during init
    if (!memory_manager::init(mem_config)) {
        Serial.println("[WARNING] Memory manager failed to initialize, continuing without it");
    }

    // 1. LED - First indicator
    g_device_state.led_ok = initLED(g_config.led_pin);

    // 2. I2C Bus - Required by multiple devices
    g_device_state.i2c_ok = initI2C(g_config.i2c_sda, g_config.i2c_scl, g_config.i2c_freq);
    if (!g_device_state.i2c_ok) {
        Serial.println("[ERROR] I2C initialization failed");
        return false;
    }

    // 3. IO Expander - Required for LCD control
    Serial.println("[DEVICE] Initializing IO Expander (EXIO)...");
    static i2c_manager::exio::State exio_state;  // Static to persist across function calls
    g_device_state.exio_ok = initEXIO(exio_state);
    g_device_state.exio_state = &exio_state;
    if (!g_device_state.exio_ok) {
        Serial.println("[ERROR] EXIO initialization failed - SYSTEM HALTED");
        Serial.println("[ERROR] This will cause buzzer stuck ON and no LCD display");
        while(1) { delay(1000); } // Halt system to prevent damage
    }
    Serial.printf("[DEVICE] EXIO initialization complete, final state: 0x%02X\n", exio_state.out);

    // 4. RTC - Optional but useful for timestamping
    g_device_state.rtc_ok = initRTC();

    // 4.5 Settings - Must load before WiFi/BLE mode decision
    settings_manager::init();
    Serial.println("[SETTINGS] Settings loaded");

    // 5. WiFi — initialized in main.cpp only when wifi_ap_enabled or wifi_sta_boot is set.
    // WiFi and NimBLE are mutually exclusive boot modes; wifi_manager::init() uses
    // default buffer sizes since NimBLE never starts in WiFi boot modes.
    // Radio stays dormant until user activates it.
    g_device_state.scanner_ok = false;

    // 6. GPS - Critical for radar application
    g_device_state.gps_ok = initGPS();

    // 6b. Compass - QMC5883L on BH-880 module (non-critical)
    g_device_state.compass_ok = initCompass();

    // 7. LCD Display - Critical component
    g_device_state.lcd_ok = initLCD(g_config);
    if (!g_device_state.lcd_ok) {
        Serial.println("[ERROR] LCD initialization failed");
        return false;
    }

    // 8. SD Card - Optional storage
    g_device_state.sd_ok = initSD(g_config);

    // 9. Backlight - Required for visibility
    g_device_state.backlight_ok = initBacklight(g_config);

    // 10. LVGL - UI framework
    g_device_state.lvgl_ok = initLVGL(g_config);
    if (!g_device_state.lvgl_ok) {
        Serial.println("[ERROR] LVGL initialization failed");
        return false;
    }

    // 11. Touch - UI input
    g_device_state.touch_ok = initTouch();

    // 12. Button - Physical button input (GPIO0)
    g_device_state.button_ok = initButton();

    // Final status
    gpio_set_level((gpio_num_t)g_config.led_pin, 0);  // Turn off init indicator
    logDeviceStatus();

    return true;
}

bool initLED(int pin) {
    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << pin;
    io.mode = GPIO_MODE_OUTPUT;
    io.pull_up_en = GPIO_PULLUP_DISABLE;
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&io);
    gpio_set_level((gpio_num_t)pin, 1);  // On during initialization
    return true;
}

bool initI2C(int sda, int scl, uint32_t freq) {
    i2c_manager::Config config;
    config.sda_pin = sda;
    config.scl_pin = scl;
    config.frequency = freq;

    return i2c_manager::init(config);
}

bool initEXIO(i2c_manager::exio::State& state) {
    Serial.println("[DEVICE] Starting EXIO initialization...");

    if (!i2c_manager::exio::begin(state)) {
        Serial.println("[EXIO] Failed to initialize");
        return false;
    }

    Serial.printf("[DEVICE] EXIO begin successful, setting initial states...\n");

    // Set initial states - BUZZER OFF is critical!
    bool buzzer_ok = i2c_manager::exio::set(i2c_manager::exio::BUZZER, false, state);
    Serial.printf("[DEVICE] Set BUZZER(pin7)=OFF: %s (state: 0x%02X)\n", buzzer_ok ? "OK" : "FAIL", state.out);

    bool lcd_cs_ok = i2c_manager::exio::set(i2c_manager::exio::LCD_CS, true, state);
    Serial.printf("[DEVICE] Set LCD_CS(pin2)=HIGH: %s (state: 0x%02X)\n", lcd_cs_ok ? "OK" : "FAIL", state.out);

    bool lcd_rst_ok1 = i2c_manager::exio::set(i2c_manager::exio::LCD_RST, false, state);
    Serial.printf("[DEVICE] Set LCD_RST(pin0)=LOW: %s (state: 0x%02X)\n", lcd_rst_ok1 ? "OK" : "FAIL", state.out);

    delay(30);

    bool lcd_rst_ok2 = i2c_manager::exio::set(i2c_manager::exio::LCD_RST, true, state);
    Serial.printf("[DEVICE] Set LCD_RST(pin0)=HIGH: %s (state: 0x%02X)\n", lcd_rst_ok2 ? "OK" : "FAIL", state.out);

    delay(150);

    if (!buzzer_ok) {
        Serial.println("[EXIO] CRITICAL: Failed to turn off buzzer!");
        return false;
    }

    Serial.println("[EXIO] Initialized successfully");
    return true;
}

bool initRTC() {
    rtc::begin(0x51);

    // Always update RTC with compile time to ensure current time
    if (rtc::set_from_compile_time()) {
        Serial.println("[RTC] Updated from compile time");
    } else {
        Serial.println("[RTC] Failed to set from compile time");
        return false;
    }

    // Test read to verify RTC is working
    rtc::Time test_time{};
    if (rtc::read(test_time) && test_time.valid) {
        Serial.printf("[RTC] Current time: %04d-%02d-%02d %02d:%02d:%02d\n",
                      test_time.year, test_time.month, test_time.day,
                      test_time.hour, test_time.minute, test_time.second);
    } else {
        Serial.println("[RTC] Warning: Time read failed or invalid");
    }

    Serial.println("[RTC] Initialized successfully");
    return true;
}

bool initScanner() {
    // Note: settings_manager::init() and wifi_manager::init() are now called
    // conditionally in initializeAll() based on wifi_enabled setting.
    // This function is kept for legacy/fallback compatibility only.
    scanner::init();
    return true;
}

bool initGPS() {
    if (!system_config::features::ENABLE_GPS) {
        Serial.println("[GPS] GPS disabled in configuration");
        return false;
    }

    Serial.println("[GPS] Initializing GNSS module...");
    Serial.printf("[GPS] UART: TX=GPIO%d, RX=GPIO%d\n",
                  system_config::pins::GPS_TX,
                  system_config::pins::GPS_RX);
    Serial.println("[GPS] Auto-detecting baud rate...");

    // Use Serial1 for GPS communication
    // Pass baud=0 to auto-detect the module's baud rate
    // BH-880 module: auto-detects between 9600 and 115200 baud
    gps_bh880::begin(0,                           // Auto-detect baud rate
                     system_config::pins::GPS_RX, // ESP32 RX = GPIO 44
                     system_config::pins::GPS_TX); // ESP32 TX = GPIO 43

    // Allow GPS module to boot before sending configuration commands
    delay(200);

    Serial.println("[GPS] BH-880 (B1301N) initialization complete");
    Serial.println("[GPS] All constellations enabled by default (GPS/GLONASS/BDS/Galileo/QZSS)");

    // ===========================================================================
    // FAST FIX: Hot/Warm Start based on saved position
    // ===========================================================================

    double saved_lat, saved_lon;
    uint32_t saved_fix_time;

    if (settings_manager::loadGPSState(saved_lat, saved_lon, saved_fix_time)) {
        rtc::Time rtc_time;
        uint32_t current_time = 0;

        if (g_device_state.rtc_ok && rtc::read(rtc_time) && rtc_time.valid) {
            struct tm tm_time = {};
            tm_time.tm_year = rtc_time.year - 1900;
            tm_time.tm_mon = rtc_time.month - 1;
            tm_time.tm_mday = rtc_time.day;
            tm_time.tm_hour = rtc_time.hour;
            tm_time.tm_min = rtc_time.minute;
            tm_time.tm_sec = rtc_time.second;
            current_time = mktime(&tm_time);
        }

        if (current_time > 0 && saved_fix_time > 0) {
            uint32_t seconds_since_fix = current_time - saved_fix_time;
            uint32_t hours_since_fix = seconds_since_fix / 3600;

            Serial.printf("[GPS] Saved position: %.6f, %.6f (%u hours ago)\n",
                         saved_lat, saved_lon, hours_since_fix);

            if (hours_since_fix < 4) {
                Serial.println("[GPS] Hot start (ephemeris valid, TTFF ~1s)");
                gps_bh880::hotStart();
            } else if (hours_since_fix < 24) {
                Serial.println("[GPS] Warm start (almanac valid, TTFF ~28s)");
                gps_bh880::warmStart();
            } else {
                Serial.println("[GPS] Data >24h old - cold start (TTFF ~28s)");
            }
        } else {
            Serial.println("[GPS] RTC unavailable - warm start with saved position");
            gps_bh880::warmStart();
        }

        delay(50);
    } else {
        Serial.println("[GPS] No saved position - cold start (TTFF ~28s)");
    }

    // Note: M10 ignores both legacy CFG-RATE and VALSET CFG-RATE-MEAS despite ACKing them.
    // Rate control is not achievable on this module. Running at default 10Hz.

    // Power mode controlled at runtime via serial: gps power full/agg1/interval

    Serial.println("[GPS] Ready - waiting for satellite fix (outdoor clear sky view required)");

    // Initialize GPS Quality Tracking (Phase 1.3)
    gps_quality::init();

    return true;
}

bool initCompass() {
    Serial.println("[COMPASS] Initializing QMC5883L on BH-880 module...");

    if (!i2c_manager::ping(i2c_manager::COMPASS_DEVICE)) {
        Serial.println("[COMPASS] Device not found at 0x0D");
        return false;
    }

    if (!compass_qmc5883l::begin()) {
        Serial.println("[COMPASS] Initialization failed");
        return false;
    }

    // Load saved hard-iron calibration offsets from NVS
    const auto& settings = settings_manager::getSettings();
    if (settings.compass_calibrated) {
        compass_qmc5883l::setCalibration(settings.compass_cal_x, settings.compass_cal_y, settings.compass_cal_z);
        Serial.printf("[COMPASS] Calibration loaded from NVS: X=%d Y=%d Z=%d\n",
                      settings.compass_cal_x, settings.compass_cal_y, settings.compass_cal_z);
    } else {
        Serial.println("[COMPASS] No calibration saved — run 'Calibrate Compass' in Settings > Display");
    }

    Serial.println("[COMPASS] Initialized successfully");
    return true;
}

bool initLCD(const Config& config) {
    // Initialize SPI for ST7701 command interface
    spi_bus_config_t buscfg = {};
    buscfg.mosi_io_num = config.lcd_mosi_pin;
    buscfg.miso_io_num = -1;
    buscfg.sclk_io_num = config.lcd_clk_pin;

    esp_err_t ret = spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        Serial.printf("[LCD] SPI bus init failed: %d\n", ret);
        return false;
    }

    spi_device_interface_config_t devcfg = {};
    devcfg.command_bits = 1;
    devcfg.address_bits = 8;
    devcfg.mode = 0;
    devcfg.clock_speed_hz = system_config::communication::SPI_FREQ_HZ;  // Configurable SPI frequency
    devcfg.spics_io_num = -1;
    devcfg.queue_size = 1;

    ret = spi_bus_add_device(SPI2_HOST, &devcfg, &g_device_state.spi_handle);
    if (ret != ESP_OK) {
        Serial.printf("[LCD] SPI device add failed: %d\n", ret);
        return false;
    }

    // Initialize ST7701 via SPI
    if (!g_device_state.exio_state) {
        Serial.println("[LCD] EXIO state not available");
        return false;
    }

    i2c_manager::exio::set(i2c_manager::exio::LCD_CS, false, *g_device_state.exio_state);
    st7701_init(g_device_state.spi_handle);
    i2c_manager::exio::set(i2c_manager::exio::LCD_CS, true, *g_device_state.exio_state);

    // Verify LCD_CS remains HIGH
    bool lcd_cs_high = (g_device_state.exio_state->out & (1u << i2c_manager::exio::LCD_CS)) != 0;
    Serial.printf("[LCD] LCD_CS after init: %s\n", lcd_cs_high ? "HIGH" : "LOW (unexpected)");

    // Initialize RGB panel
    esp_lcd_rgb_panel_config_t cfg = {};
    cfg.data_width = 16;
    cfg.clk_src = LCD_CLK_SRC_PLL160M;
    cfg.dma_burst_size = 64;  // IDF 5.5: replaces deprecated psram_trans_align
    cfg.pclk_gpio_num = config.lcd_pclk;
    cfg.vsync_gpio_num = config.lcd_vsync;
    cfg.hsync_gpio_num = config.lcd_hsync;
    cfg.de_gpio_num = config.lcd_de;

    for (int i = 0; i < 16; i++) {
        cfg.data_gpio_nums[i] = DATA_PINS[i];
    }

    cfg.timings.h_res = config.screen_width;
    cfg.timings.v_res = config.screen_height;
    cfg.timings.pclk_hz = config.pclk_hz;
    cfg.timings.hsync_pulse_width = 8;
    cfg.timings.hsync_back_porch = 20;
    cfg.timings.hsync_front_porch = 20;
    cfg.timings.vsync_pulse_width = 4;
    cfg.timings.vsync_back_porch = 8;
    cfg.timings.vsync_front_porch = 10;
    cfg.timings.flags.hsync_idle_low = 0;
    cfg.timings.flags.vsync_idle_low = 0;
    cfg.timings.flags.de_idle_high = 0;
    cfg.timings.flags.pclk_active_neg = 0;
    cfg.timings.flags.pclk_idle_high = 0;
    cfg.flags.fb_in_psram = 1;
    cfg.bounce_buffer_size_px = system_config::display::SCREEN_WIDTH * system_config::display::BOUNCE_BUFFER_LINES;  // IDF 5.5: SRAM staging eliminates PSRAM/DMA bus contention

    Serial.printf("[RGB] %dx%d pclk=%lu Hz | H:pw=%u bp=%u fp=%u | V:pw=%u bp=%u fp=%u\n",
                  cfg.timings.h_res, cfg.timings.v_res, (unsigned)cfg.timings.pclk_hz,
                  (unsigned)cfg.timings.hsync_pulse_width, (unsigned)cfg.timings.hsync_back_porch, (unsigned)cfg.timings.hsync_front_porch,
                  (unsigned)cfg.timings.vsync_pulse_width, (unsigned)cfg.timings.vsync_back_porch, (unsigned)cfg.timings.vsync_front_porch);

    ret = esp_lcd_new_rgb_panel(&cfg, &g_device_state.panel_handle);
    if (ret != ESP_OK) {
        Serial.printf("[LCD] RGB panel creation failed: %d\n", ret);
        return false;
    }

    ret = esp_lcd_panel_reset(g_device_state.panel_handle);
    if (ret != ESP_OK) {
        Serial.printf("[LCD] Panel reset failed: %d\n", ret);
        return false;
    }

    ret = esp_lcd_panel_init(g_device_state.panel_handle);
    if (ret != ESP_OK) {
        Serial.printf("[LCD] Panel init failed: %d\n", ret);
        return false;
    }

    // Register panel callbacks (ESP-IDF 5.x API)
    // on_color_trans_done: DMA complete → LVGL flush_ready
    // on_vsync:            Frame boundary → unblock uiTask for tear-free rendering
    g_vsync_sem = xSemaphoreCreateBinary();
    esp_lcd_rgb_panel_event_callbacks_t cbs = {};
    cbs.on_color_trans_done = on_color_trans_done;
    cbs.on_vsync = on_vsync_cb;
    esp_lcd_rgb_panel_register_event_callbacks(g_device_state.panel_handle, &cbs, nullptr);

    Serial.println("[LCD] Display initialized successfully");
    return true;
}

bool initSD(const Config& config) {
    if (!g_device_state.exio_state) {
        Serial.println("[SD] EXIO state not available");
        return false;
    }

    // Enable SD card power via EXIO4
    i2c_manager::exio::set(i2c_manager::exio::EXIO4, true, *g_device_state.exio_state);
    delay(10);

    // SDMMC host in 1-bit mode (pins shared with LCD command SPI)
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.flags = SDMMC_HOST_FLAG_1BIT;
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.clk   = (gpio_num_t)config.lcd_clk_pin;   // GPIO2
    slot_config.cmd   = (gpio_num_t)config.lcd_mosi_pin;  // GPIO1
    slot_config.d0    = GPIO_NUM_42;
    slot_config.width = 1;
    slot_config.flags = 0;  // No CD/WP pins

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {};
    mount_config.format_if_mount_failed = false;
    mount_config.max_files = 8;
    mount_config.allocation_unit_size = 16 * 1024;

    esp_err_t ret = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot_config,
                                             &mount_config, &g_device_state.sdmmc_card);
    if (ret != ESP_OK) {
        Serial.printf("[SD] Mount failed: %s\n", esp_err_to_name(ret));
        return false;
    }

    // Compute card size from CSD
    sdmmc_card_t* card = g_device_state.sdmmc_card;
    uint64_t total_bytes = (uint64_t)card->csd.capacity * card->csd.sector_size;
    g_device_state.sd_mb = (uint32_t)(total_bytes / (1024 * 1024));
    Serial.printf("[SD] Mounted successfully: %u MB\n", (unsigned)g_device_state.sd_mb);

    return true;
}

bool initBacklight(const Config& config) {
    backlight::Cfg bl;
    bl.pin = config.lcd_bl;
    bl.ledcChan = 0;
    bl.ledcTimer = 0;
    bl.freqHz = config.backlight_freq;
    bl.resBits = 8;
    bl.usePwm = true;

    if (!backlight::begin(bl)) {
        Serial.println("[BACKLIGHT] Initialization failed");
        return false;
    }

    // Set initial brightness
    const uint8_t init_duty = (uint8_t)((config.backlight_init_percent * 255 + 50) / 100);
    backlight::set(init_duty);

    Serial.printf("[BACKLIGHT] Initialized: %d%% brightness\n", config.backlight_init_percent);
    return true;
}

bool initLVGL(const Config& config) {
    lv_init();

    // Allocate LVGL buffers in PSRAM
    const size_t buf_size = config.screen_width * config.buffer_lines * sizeof(lv_color_t);
    lv_buf1 = (lv_color_t*)heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    lv_buf2 = (lv_color_t*)heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);

    if (!lv_buf1 || !lv_buf2) {
        Serial.println("[LVGL] ERROR: failed to allocate frame buffers");
        return false;
    }

    lv_disp_draw_buf_init(&draw_buf, lv_buf1, lv_buf2, config.screen_width * config.buffer_lines);
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = config.screen_width;
    disp_drv.ver_res = config.screen_height;
    disp_drv.flush_cb = lvgl_flush_cb;
    disp_drv.draw_buf = &draw_buf;

    // Partial refresh for performance (120-line buffers provide smooth scrolling)
    disp_drv.full_refresh = 0;          // Partial refresh is faster - only redraws changed areas

    // Enable software rotation for RGB panel (MUST be set BEFORE registration)
    #if LV_VERSION_CHECK(8,0,0)
        if (system_config::display::ROTATION_DEGREES == 90) {
            disp_drv.sw_rotate = 1;
            disp_drv.rotated = LV_DISP_ROT_90;
            Serial.println("[LVGL] Software rotation enabled: 90° CW (compensating for physical 90° CCW)");
        } else if (system_config::display::ROTATION_DEGREES == 180) {
            disp_drv.sw_rotate = 1;
            disp_drv.rotated = LV_DISP_ROT_180;
            Serial.println("[LVGL] Software rotation enabled: 180°");
        } else if (system_config::display::ROTATION_DEGREES == 270) {
            disp_drv.sw_rotate = 1;
            disp_drv.rotated = LV_DISP_ROT_270;
            Serial.println("[LVGL] Software rotation enabled: 270° CW (90° CCW)");
        } else {
            Serial.println("[LVGL] No display rotation applied");
        }
    #else
        #warning "LVGL rotation requires LVGL 8.0.0 or higher"
        Serial.println("[LVGL] WARNING: Display rotation not supported in LVGL < 8.0.0");
    #endif

    Serial.printf("[LVGL] FB0=%p FB1=%p | BUF_LINES=%d\n", (void*)lv_buf1, (void*)lv_buf2, config.buffer_lines);

    lv_disp_drv_register(&disp_drv);

    return true;
}

bool initTouch() {
    if (!cst820_begin(0x15)) {
        Serial.println("[TOUCH] CST820 not found");
        return false;
    }

    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = lvgl_touch_read_cb;
    lv_indev_drv_register(&indev_drv);

    Serial.println("[TOUCH] Initialized successfully");
    return true;
}

bool initButton() {
    // Configure button on GPIO0 with hardware pull-up
    button::Config btn_config;
    btn_config.debounce_ms = 20;              // 20ms: safe for tactile switches (most bounce <5ms)
    btn_config.long_press_ms = 2000;
    btn_config.double_press_window_ms = 300;  // 300ms: fast enough for double-tap, short enough for snappy single-press

    if (!button::begin(GPIO_NUM_0, btn_config)) {
        Serial.println("[BUTTON] Failed to initialize");
        return false;
    }

    // Register button event callback
    // CRITICAL: This callback runs on UI Task (Core 1) during button::update()
    // To avoid race conditions with lv_timer_handler(), we queue ALL UI updates
    // instead of calling LVGL directly. This prevents:
    // 1. LoadProhibited crash from concurrent LVGL access
    // 2. UI_Task freeze from unprotected LVGL calls in standby functions
    button::setEventCallback([](button::Event event) {
        // Track when we last woke from standby to suppress the trailing
        // SINGLE_PRESS that fires ~400ms after the RELEASED event that woke us
        static uint32_t wake_time_ms = 0;

        // Check if waking from standby - any button press wakes
        if (standby_manager::isStandby()) {
            Serial.println("[BUTTON] Wake from standby - queueing wake request");
            system_logger::info("BUTTON", "Button press during standby - queued WAKE_STANDBY");
            wake_time_ms = millis();
            // CRITICAL FIX: Queue wake instead of direct call (was causing UI freeze)
            task_manager::UIUpdate update;
            update.type = task_manager::UIUpdateType::WAKE_STANDBY;
            update.timestamp = millis();
            if (!task_manager::queueUIUpdate(update)) {
                Serial.println("[BUTTON] WARNING: Failed to queue wake - trying direct wake");
                system_logger::warn("BUTTON", "Queue FULL - fallback to direct wake");
                // Fallback: direct wake if queue fails (better than no wake)
                standby_manager::wakeFromStandby();
            }
            return;  // Don't process other button events during wake
        }

        // Suppress button events shortly after wake (the SINGLE_PRESS that
        // fires after the double-press window expires from the wake press)
        if (wake_time_ms > 0 && (millis() - wake_time_ms) < 1200) {
            Serial.println("[BUTTON] Suppressed post-wake button event");
            return;
        }
        wake_time_ms = 0;

        switch (event) {
            case button::Event::SINGLE_PRESS:
                {
                    // Suppress zoom while settings screen is visible
                    {
                        const ui_manager::UIState& ui_s = ui_manager::getUIState();
                        if (ui_s.screen_settings && lv_scr_act() == ui_s.screen_settings) {
                            Serial.println("[BUTTON] Single press ignored — settings screen active");
                            break;
                        }
                    }
                    Serial.println("[BUTTON] Single press - queueing zoom change");
                    system_logger::info("BUTTON", "Single press - queued ZOOM_CHANGE");
                    // Queue zoom change instead of direct LVGL call (thread-safe)
                    task_manager::UIUpdate update;
                    update.type = task_manager::UIUpdateType::ZOOM_CHANGE;
                    update.timestamp = millis();
                    if (!task_manager::queueUIUpdate(update)) {
                        Serial.println("[BUTTON] WARNING: Failed to queue zoom change");
                        system_logger::error("BUTTON", "Queue FULL - dropped ZOOM_CHANGE");
                    }
                }
                break;

            case button::Event::DOUBLE_PRESS:
                {
                    // Suppress zoom while settings screen is visible
                    {
                        const ui_manager::UIState& ui_s = ui_manager::getUIState();
                        if (ui_s.screen_settings && lv_scr_act() == ui_s.screen_settings) {
                            Serial.println("[BUTTON] Double press ignored — settings screen active");
                            break;
                        }
                    }
                    Serial.println("[BUTTON] Double press - queueing zoom out");
                    system_logger::info("BUTTON", "Double press - queued ZOOM_CHANGE_REVERSE");
                    // Queue zoom change (reverse) instead of direct LVGL call (thread-safe)
                    task_manager::UIUpdate update;
                    update.type = task_manager::UIUpdateType::ZOOM_CHANGE_REVERSE;
                    update.timestamp = millis();
                    if (!task_manager::queueUIUpdate(update)) {
                        Serial.println("[BUTTON] WARNING: Failed to queue zoom reverse");
                        system_logger::error("BUTTON", "Queue FULL - dropped ZOOM_CHANGE_REVERSE");
                    }
                }
                break;

            case button::Event::LONG_PRESS:
                {
                    Serial.println("[BUTTON] Long press (2s) - queueing settings screen");
                    system_logger::info("BUTTON", "Long press (2s) - queued SETTINGS_SCREEN");
                    // Queue settings screen navigation instead of direct call (thread-safe)
                    task_manager::UIUpdate update;
                    update.type = task_manager::UIUpdateType::SETTINGS_SCREEN;
                    update.timestamp = millis();
                    if (!task_manager::queueUIUpdate(update)) {
                        Serial.println("[BUTTON] WARNING: Failed to queue settings screen");
                        system_logger::error("BUTTON", "Queue FULL - dropped SETTINGS_SCREEN");
                    }
                }
                break;

            case button::Event::EXTRA_LONG_PRESS:
                {
                    Serial.println("[BUTTON] Extra-long press (4s) - queueing standby mode");
                    system_logger::info("BUTTON", "Extra-long press (4s) - queued ENTER_STANDBY");
                    // CRITICAL FIX: Queue standby instead of direct call (was causing UI freeze)
                    task_manager::UIUpdate update;
                    update.type = task_manager::UIUpdateType::ENTER_STANDBY;
                    update.timestamp = millis();
                    if (!task_manager::queueUIUpdate(update)) {
                        Serial.println("[BUTTON] WARNING: Failed to queue standby - trying direct");
                        system_logger::warn("BUTTON", "Queue FULL - fallback to direct standby");
                        // Fallback: direct call if queue fails
                        standby_manager::enterStandby();
                    }
                }
                break;

            case button::Event::RELEASED:
                // Optionally handle button release
                break;

            default:
                break;
        }
    });

    Serial.println("[BUTTON] Initialized on GPIO0 with event handlers");

    // Initialize buzzer subsystem (uses I2C EXIO, must be after I2C init)
    if (!buzzer::init()) {
        Serial.println("[BUZZER] Warning: Buzzer initialization failed");
        // Non-critical - continue anyway
    }

    return true;
}

void updateButton() {
    button::update();
    // NOTE: buzzer::update() is driven by I2C Task (task_manager.cpp), NOT here.
    // Calling it from both UI Task and I2C Task caused race conditions on g_state.
}

void enableLVGLProcessing() {
    g_lvgl_unlocked = true;
    Serial.println("[LVGL] Processing enabled");
}

bool isLVGLUnlocked() {
    return g_lvgl_unlocked;
}

void updateSDStatus() {
    // Update SD status in device state if needed
    // This can be called periodically to check SD card health
}

void logDeviceStatus() {
    Serial.println("==== Device Status Summary ====");
    Serial.printf("LED:       %s\n", g_device_state.led_ok ? "OK" : "FAIL");
    Serial.printf("I2C:       %s\n", g_device_state.i2c_ok ? "OK" : "FAIL");
    Serial.printf("EXIO:      %s\n", g_device_state.exio_ok ? "OK" : "FAIL");
    Serial.printf("RTC:       %s\n", g_device_state.rtc_ok ? "OK" : "FAIL");
    Serial.printf("Scanner:   %s\n", g_device_state.scanner_ok ? "OK" : "FAIL");
    Serial.printf("GPS:       %s\n", g_device_state.gps_ok ? "OK" : "FAIL");
    Serial.printf("Compass:   %s\n", g_device_state.compass_ok ? "OK" : "FAIL");
    Serial.printf("LCD:       %s\n", g_device_state.lcd_ok ? "OK" : "FAIL");
    Serial.printf("SD:        %s", g_device_state.sd_ok ? "OK" : "FAIL");
    if (g_device_state.sd_ok) Serial.printf(" (%u MB)", (unsigned)g_device_state.sd_mb);
    Serial.println();
    Serial.printf("Backlight: %s\n", g_device_state.backlight_ok ? "OK" : "FAIL");
    Serial.printf("LVGL:      %s\n", g_device_state.lvgl_ok ? "OK" : "FAIL");
    Serial.printf("Touch:     %s\n", g_device_state.touch_ok ? "OK" : "FAIL");
    Serial.printf("Button:    %s\n", g_device_state.button_ok ? "OK" : "FAIL");
    Serial.println("===============================");
}

// LVGL callback implementations
// DMA callback - called when frame transfer actually completes
static bool IRAM_ATTR on_color_trans_done(esp_lcd_panel_handle_t panel, const esp_lcd_rgb_panel_event_data_t* edata, void* user_ctx) {
    // Notify LVGL that flush is complete - NOW it's safe to start next frame
    if (g_current_disp_drv) {
        lv_disp_flush_ready(g_current_disp_drv);
    }
    return false;  // No high-priority task woken
}

// VSYNC callback - fires at the start of every hardware frame (~37.6 Hz, ~26.6ms period).
// Unblocks uiTask so lv_timer_handler() runs at frame boundaries → tear-free rendering.
// Phase 3 — Item 3: VSYNC flush scheduling (tickets/ui_espidf_improvements.md)
static bool IRAM_ATTR on_vsync_cb(esp_lcd_panel_handle_t panel, const esp_lcd_rgb_panel_event_data_t* edata, void* user_ctx) {
    BaseType_t hp_woken = pdFALSE;
    if (g_vsync_sem) {
        xSemaphoreGiveFromISR(g_vsync_sem, &hp_woken);
    }
    return hp_woken == pdTRUE;  // Yield to higher-priority task if woken
}

SemaphoreHandle_t getVsyncSemaphore() {
    return g_vsync_sem;
}

static void lvgl_flush_cb(lv_disp_drv_t* drv, const lv_area_t* area, lv_color_t* color_p) {
    // Check if diagnostics wants to skip flush (freeze feature)
    if (diagnostics::shouldSkipLVGLFlush()) {
        lv_disp_flush_ready(drv);
        return;
    }

    // Store driver pointer for DMA callback
    g_current_disp_drv = drv;

    // Start DMA transfer (asynchronous - callback will fire when done)
    esp_lcd_panel_draw_bitmap(g_device_state.panel_handle, area->x1, area->y1, area->x2+1, area->y2+1, color_p);

    // Increment FPS counter for navigation module
    navigation::getNavState().flush_count = navigation::getNavState().flush_count + 1;

    // CRITICAL: Do NOT call lv_disp_flush_ready() here!
    // The on_color_trans_done callback will call it when DMA actually finishes
}

static inline int16_t scale_to_480(uint16_t raw) {
    if (raw <= 600)  return (raw > 479) ? 479 : raw;
    if (raw <= 1500) return (int32_t)raw * 480 / 1023;
    return (int32_t)raw * 480 / 4095;
}

static void lvgl_touch_read_cb(lv_indev_drv_t*, lv_indev_data_t* data) {
    CST820Point pt;
    static int16_t last_x = 240, last_y = 240;  // Screen center

    // Don't poll the CST820 during standby — continuous I2C reads at 100Hz
    // with the display off corrupt the touch controller's state over time.
    if (standby_manager::isStandby()) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    if (cst820_read(pt, 0x15) && pt.pressed) {
        int16_t lx = scale_to_480(pt.x);
        int16_t ly = scale_to_480(pt.y);

        // Clamp to screen bounds
        if (lx < 0) lx = 0; else if (lx >= g_config.screen_width) lx = g_config.screen_width - 1;
        if (ly < 0) ly = 0; else if (ly >= g_config.screen_height) ly = g_config.screen_height - 1;

        // Circle mask: ignore touches outside circular display area
        int dx = lx - g_config.screen_width/2;
        int dy = ly - g_config.screen_height/2;
        const int R = (g_config.screen_width/2) - 2;  // Slightly smaller than actual radius

        if ((dx*dx + dy*dy) > R*R) {
            data->state = LV_INDEV_STATE_RELEASED;
            data->point.x = last_x;
            data->point.y = last_y;

            // Update navigation state
            navigation::getNavState().touch_pressed = false;
            return;
        }

        standby_manager::notifyUserActivity();  // Reset inactivity timer
        data->state = LV_INDEV_STATE_PRESSED;
        data->point.x = last_x = lx;
        data->point.y = last_y = ly;

        // Update navigation state
        navigation::getNavState().touch_x = lx;
        navigation::getNavState().touch_y = ly;
        navigation::getNavState().touch_pressed = true;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
        data->point.x = last_x;
        data->point.y = last_y;

        // Update navigation state
        navigation::getNavState().touch_pressed = false;
    }
}

} // namespace device_manager