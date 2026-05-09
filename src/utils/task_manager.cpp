#include "task_manager.h"
#include "system_config.h"
#include "utils/wmm_declination.h"
#include "navigation.h"
#include "device_manager.h"
#include "i2c_manager.h"
#include "scanner.h"
#include "memory_manager.h"
#include "diagnostics.h"
#include "ui_manager.h"
#include "settings_manager.h"
#include "system_logger.h"
#include "ntp_sync.h"
#include "rtc_pcf85063.h"
#include "gps_bh880.h"
#include "compass_qmc5883l.h"
#include "navigation/gps_quality.h"
#include "hardware/sensors/battery.h"
#include "standby_manager.h"
#include "utils/watchdog.h"
#include "hardware/connectivity/beacon_proximity.h"
#include "hardware/buzzer.h"
#include "hardware/connectivity/wifi_manager.h"
#include "gpx/gpx_server.h"
#include "esp_heap_caps.h"
#include <lvgl.h>

namespace task_manager {

// =============================================================================
// GLOBAL STATE
// =============================================================================

// Task handles
TaskHandle_t ui_task_handle = nullptr;
TaskHandle_t i2c_task_handle = nullptr;
TaskHandle_t network_task_handle = nullptr;
TaskHandle_t system_task_handle = nullptr;

// Synchronization primitives
QueueHandle_t i2c_request_queue = nullptr;
QueueHandle_t ui_update_queue = nullptr;
SemaphoreHandle_t display_mutex = nullptr;
SemaphoreHandle_t i2c_mutex = nullptr;
SemaphoreHandle_t ui_state_mutex = nullptr;

// Task control
static bool tasks_running = false;
static bool task_enabled[4] = {true, true, true, true}; // UI, I2C, Network, System - All enabled

// Statistics
static SystemStats system_stats = {};

// Timing for legacy support
// last_tick removed — LV_TICK_CUSTOM handles tick via esp_timer_get_time()

// =============================================================================
// FORWARD DECLARATIONS
// =============================================================================

static void processI2CRequest(I2CRequest& request);
static void processUIUpdate(const UIUpdate& update);
static void updateMemoryStats();
static void updateStatusLabels();
static void checkTaskHealth();

// =============================================================================
// ZOOM-BASED FEATURE ACTIVATION (50m zoom only)
// =============================================================================

/**
 * @brief Update zoom-dependent features based on current zoom level
 *
 * Beacon proximity is only active at 50m zoom.
 * This saves resources during general navigation at coarser zoom levels.
 */
static void updateZoomDependentFeatures() {
    const auto& settings = settings_manager::getSettings();
    ui_manager::UIState& ui = ui_manager::getUIState();
    bool at_50m_zoom = (ui.current_zoom == ui_manager::ZoomLevel::ZOOM_50M);

    // === BEACON PROXIMITY ACTIVATION ===
    if (settings.beacon_proximity_enabled) {
        bool beacon_scanning = beacon_proximity::isEnabled();

        if (at_50m_zoom && !beacon_scanning) {
            // Entering 50m zoom - start beacon proximity scanning.
            // WiFi STA fragments DMA SRAM permanently (esp_wifi_stop doesn't free it).
            // If WiFi was ever active this session, NimBLE will fail to allocate.
            if (wifi_manager::wasEverEnabled()) {
                Serial.println("[BEACON] Skipping beacon at 50m — WiFi was used this session (reboot to enable)");
            } else {
                Serial.println("[BEACON] Entering 50m zoom - starting beacon scanning");
                beacon_proximity::setEnabled(true);
            }
        } else if (!at_50m_zoom && beacon_scanning) {
            // Leaving 50m zoom - stop beacon scanning and hide dBm overlay
            Serial.println("[BEACON] Leaving 50m zoom - stopping beacon scanning");
            beacon_proximity::setEnabled(false);
            // Hide dBm label immediately (no new BEACON_DBM_UPDATE will be queued)
            if (ui.beacon_dbm_label && lv_obj_is_valid(ui.beacon_dbm_label)) {
                lv_obj_add_flag(ui.beacon_dbm_label, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}

// =============================================================================
// TASK IMPLEMENTATIONS
// =============================================================================

/**
 * @brief UI Task - Handles LVGL processing and touch input
 * Runs on Core 1 with highest priority for responsiveness
 *
 * THREAD SAFETY: All LVGL operations are protected by display_mutex.
 * This prevents race conditions between lv_timer_handler() and
 * queued UI updates from button callbacks.
 */
static void uiTask(void* parameter) {
    Serial.println("[TASK] UI Task started on Core 1");
    system_logger::info("UI_TASK", "Started on Core 1");
    system_stats.ui_task.is_healthy = true;
    strcpy(system_stats.ui_task.status_message, "Running");

    // Subscribe to Task Watchdog Timer (TWDT)
    if (watchdog::isInitialized()) {
        if (watchdog::subscribe()) {
            Serial.println("[TASK] UI Task subscribed to watchdog");
            system_logger::debug("UI_TASK", "Subscribed to watchdog");
        }
    }

    TickType_t last_wake = xTaskGetTickCount();

    while (tasks_running && task_enabled[0]) {
        uint32_t loop_start = millis();
        system_stats.ui_task.loop_count++;

        // DEV MODE: Checkpoint logging every 100 loops

        // Feed watchdog to prevent timeout
        watchdog::feed();

        // CRITICAL: Poll button BEFORE queue processing for responsive input
        // Button callbacks queue UI updates, which are processed below with mutex held
        // This ensures button presses are detected immediately, not after 1+ second
        // queue drain (each RADAR_REFRESH takes ~149ms × 8 queue items = 1.2s delay)
        device_manager::updateButton();

        // CRITICAL: Acquire mutex before ANY LVGL operations
        // This protects against race conditions with UI updates from other sources
        // Timeout increased to 300ms to accommodate worst-case LVGL operations
        if (xSemaphoreTake(display_mutex, pdMS_TO_TICKS(300)) == pdTRUE) {
            // Process LVGL tasks (timer handlers, animations, etc.)
            if (device_manager::isLVGLUnlocked()) {
                lv_timer_handler();
            }

            // Auto-hide HUD after inactivity timeout (moved from app_main loop)
            ui_manager::updateHUDVisibility();

            // Process UI update queue WHILE HOLDING MUTEX
            // LIMIT: Process max 4 items per loop to prevent mutex starvation
            // This ensures display_mutex is released regularly for other tasks
            UIUpdate update;
            int processed = 0;
            constexpr int MAX_QUEUE_ITEMS_PER_LOOP = 4;
            while (processed < MAX_QUEUE_ITEMS_PER_LOOP &&
                   xQueueReceive(ui_update_queue, &update, 0) == pdTRUE) {
                processUIUpdate(update);
                system_stats.ui_updates_queued++;
                processed++;
            }

            xSemaphoreGive(display_mutex);
        } else {
            // Mutex timeout - log but continue (should be rare)
            static uint32_t mutex_timeout_count = 0;
            mutex_timeout_count++;
            if (mutex_timeout_count % 100 == 1) {
                Serial.printf("[TASK] UI Task mutex timeout #%lu\n", mutex_timeout_count);
            }
            // DEV MODE: Log every mutex timeout
        }


        // Calculate task statistics
        uint32_t loop_end = millis();
        system_stats.ui_task.last_runtime_ms = loop_end - loop_start;
        system_stats.ui_task.stack_high_water = uxTaskGetStackHighWaterMark(nullptr);
        system_stats.ui_task.health.last_loop_time_ms = loop_end;

        // Gate rendering to vsync boundaries for tear-free output (~37.6 Hz, ~26.6ms/frame).
        // on_vsync_cb (ISR) gives this semaphore at the start of each hardware frame.
        // 30ms timeout = ~3 missed vsyncs — fallback to timed delay if vsync not available.
        SemaphoreHandle_t vsync_sem = device_manager::getVsyncSemaphore();
        if (vsync_sem) {
            xSemaphoreTake(vsync_sem, pdMS_TO_TICKS(30));
        } else {
            vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(TaskConfig::UI_UPDATE_MS));
        }
    }

    // Unsubscribe from watchdog before exit
    if (watchdog::isInitialized()) {
        watchdog::unsubscribe();
    }

    Serial.println("[TASK] UI Task stopped");
    strcpy(system_stats.ui_task.status_message, "Stopped");
    vTaskDelete(nullptr);
}

/**
 * @brief I2C Task - Handles all I2C device communication
 * Runs on Core 0 with medium priority, processes queued requests
 */
static void i2cTask(void* parameter) {
    Serial.println("[TASK] I2C Task started on Core 0");
    system_stats.i2c_task.is_healthy = true;
    strcpy(system_stats.i2c_task.status_message, "Running");

    // Subscribe to Task Watchdog Timer (TWDT)
    if (watchdog::isInitialized()) {
        if (watchdog::subscribe()) {
            Serial.println("[TASK] I2C Task subscribed to watchdog");
        }
    }

    TickType_t last_wake = xTaskGetTickCount();

    while (tasks_running && task_enabled[1]) {
        uint32_t loop_start = millis();
        system_stats.i2c_task.loop_count++;

        // Feed watchdog to prevent timeout
        watchdog::feed();

        // Process I2C request queue
        I2CRequest request;
        while (xQueueReceive(i2c_request_queue, &request, 0) == pdTRUE) {
            system_stats.total_i2c_requests++;

            // i2c_manager::read/write acquire i2c_mutex internally
            processI2CRequest(request);

            // Notify requester if callback provided
            if (request.callback) {
                request.callback(request);
            }
        }

        // Drive buzzer state machine: sonar beep timing, pattern steps, auto-off
        // Must run from I2C Task since buzzer uses EXIO over I2C.
        // 20ms loop gives precise sonar rhythm (was never called before — root cause of erratic beeping).
        buzzer::update();

        // Calculate task statistics
        uint32_t loop_end = millis();
        system_stats.i2c_task.last_runtime_ms = loop_end - loop_start;
        system_stats.i2c_task.stack_high_water = uxTaskGetStackHighWaterMark(nullptr);
        system_stats.i2c_task.health.last_loop_time_ms = loop_end;

        // Task delay
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(TaskConfig::I2C_PROCESS_MS));
    }

    // Unsubscribe from watchdog before exit
    if (watchdog::isInitialized()) {
        watchdog::unsubscribe();
    }

    Serial.println("[TASK] I2C Task stopped");
    strcpy(system_stats.i2c_task.status_message, "Stopped");
    vTaskDelete(nullptr);
}

/**
 * @brief Network Task - Handles WiFi/BLE scanning
 * Runs on Core 0 with low priority, non-blocking operations
 */
static void networkTask(void* parameter) {
    Serial.println("[TASK] Network Task started on Core 0");
    system_stats.network_task.is_healthy = true;
    strcpy(system_stats.network_task.status_message, "Running");

    // Subscribe to Task Watchdog Timer (TWDT)
    if (watchdog::isInitialized()) {
        if (watchdog::subscribe()) {
            Serial.println("[TASK] Network Task subscribed to watchdog");
        }
    }

    TickType_t last_wake = xTaskGetTickCount();

    while (tasks_running && task_enabled[2]) {
        uint32_t loop_start = millis();
        system_stats.network_task.loop_count++;

        // Feed watchdog to prevent timeout
        watchdog::feed();

        // Update scanner (WiFi/BLE)
        scanner::update();

        // Update beacon proximity - BLE scanning and distance calculation
        beacon_proximity::update();

        // Update sonar beeping interval based on current zone
        beacon_proximity::updateSonar();

        // GPX server auto-start/stop when WiFi STA connects/disconnects.
        // AP mode is managed exclusively by settings_screen — this block must NOT touch
        // the server when AP is active, otherwise it races with the settings toggle.
        {
            static bool gpx_server_started = false;
            const bool ap_mode_enabled = settings_manager::getSettings().wifi_ap_enabled;

            if (!ap_mode_enabled) {
                // STA mode: auto-start server when connected + IP assigned
                const bool sta_connected = wifi_manager::isConnected();
                const bool has_ip = sta_connected
                                    && wifi_manager::getIPAddress() != "0.0.0.0";

                if (!gpx_server_started && has_ip) {
                    Serial.println("[GPX] WiFi connected, starting web server...");
                    if (gpx_server::start()) {
                        gpx_server_started = true;
                        char ip[16];
                        if (gpx_server::getStatus(ip, sizeof(ip))) {
                            Serial.printf("[GPX] Web portal ready at: http://%s\n", ip);
                        }
                    } else {
                        Serial.println("[GPX] Failed to start web server");
                    }
                } else if (gpx_server_started && !sta_connected) {
                    Serial.println("[GPX] WiFi disconnected, stopping web server...");
                    gpx_server::stop();
                    gpx_server_started = false;
                }
            } else {
                // AP mode: settings_screen owns the server; reset flag so auto-start
                // re-arms correctly if user later switches to STA mode
                gpx_server_started = false;
            }
        }

        // Calculate task statistics
        uint32_t loop_end = millis();
        system_stats.network_task.last_runtime_ms = loop_end - loop_start;
        system_stats.network_task.stack_high_water = uxTaskGetStackHighWaterMark(nullptr);
        system_stats.network_task.health.last_loop_time_ms = loop_end;

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(TaskConfig::NETWORK_UPDATE_MS));
    }

    // Unsubscribe from watchdog before exit
    if (watchdog::isInitialized()) {
        watchdog::unsubscribe();
    }

    Serial.println("[TASK] Network Task stopped");
    strcpy(system_stats.network_task.status_message, "Stopped");
    vTaskDelete(nullptr);
}

/**
 * @brief System Task - Handles memory monitoring and diagnostics
 * Runs on Core 0 with low priority, background operations
 */
static void systemTask(void* parameter) {
    Serial.println("[TASK] System Task started on Core 0");
    system_stats.system_task.is_healthy = true;
    strcpy(system_stats.system_task.status_message, "Running");

    // Subscribe to Task Watchdog Timer (TWDT)
    if (watchdog::isInitialized()) {
        if (watchdog::subscribe()) {
            Serial.println("[TASK] System Task subscribed to watchdog");
        }
    }

    TickType_t last_wake = xTaskGetTickCount();

    while (tasks_running && task_enabled[3]) {
        uint32_t loop_start = millis();
        system_stats.system_task.loop_count++;

        // Feed watchdog to prevent timeout
        watchdog::feed();

        // Process diagnostic commands
        diagnostics::processCommands();

        // Memory monitoring (re-enable memory manager safely)
        static uint32_t last_memory_check = 0;
        uint32_t now = millis();
        if (now - last_memory_check >= 30000) { // Every 30 seconds
            last_memory_check = now;
            updateMemoryStats();
        }

        // SD card status update
        static uint32_t last_sd_check = 0;
        if (now - last_sd_check >= 10000) { // Every 10 seconds
            last_sd_check = now;
            device_manager::updateSDStatus();
        }

        // System health monitoring
        checkTaskHealth();

        updateStatusLabels();

        // Get device state (needed by multiple sections below)
        const device_manager::DeviceState& dev_state = device_manager::getDeviceState();

        // Battery monitoring - update voltage history and periodic monitoring
        battery::update();                  // Collect voltage samples for trend analysis
        battery::updatePeriodicMonitoring();  // Print status if monitoring enabled

        // Auto-sleep: check if inactivity timeout has elapsed
        standby_manager::checkInactivityTimeout();

        // =========================================================================
        // PERIODIC LOG FLUSH (every 30 seconds)
        // =========================================================================
        if (system_logger::needsFlush()) {
            system_logger::flush();
        }

        // PERIODIC HEARTBEAT LOG (every 2 minutes)
        // =========================================================================
        static uint32_t last_heartbeat_log = 0;
        if (now - last_heartbeat_log >= 120000) {
            last_heartbeat_log = now;

            uint32_t uptime_min = now / 60000;
            uint32_t heap_free = esp_get_free_heap_size();

            char heartbeat_msg[128];
            snprintf(heartbeat_msg, sizeof(heartbeat_msg),
                     "Up=%lum Heap=%lu UI=%s I2C=%s GPS=%s",
                     uptime_min, heap_free,
                     system_stats.ui_task.is_healthy ? "OK" : "FAIL",
                     system_stats.i2c_task.is_healthy ? "OK" : "FAIL",
                     dev_state.last_gps_data.valid ? "FIX" : "NO");

            system_logger::info("HEALTH", heartbeat_msg);
        }

        // Update peripheral health monitoring

        // Calculate task statistics
        uint32_t loop_end = millis();
        system_stats.system_task.last_runtime_ms = loop_end - loop_start;
        system_stats.system_task.stack_high_water = uxTaskGetStackHighWaterMark(nullptr);
        system_stats.system_task.health.last_loop_time_ms = loop_end;

        // Task delay (5 second intervals)
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(TaskConfig::SYSTEM_UPDATE_MS));
    }

    // Unsubscribe from watchdog before exit
    if (watchdog::isInitialized()) {
        watchdog::unsubscribe();
    }

    Serial.println("[TASK] System Task stopped");
    strcpy(system_stats.system_task.status_message, "Stopped");
    vTaskDelete(nullptr);
}

// =============================================================================
// HELPER FUNCTIONS
// =============================================================================

static void processI2CRequest(I2CRequest& request) {
    request.success = false;

    switch (request.operation) {
        case I2COperation::READ:
            if (request.device == I2CDeviceType::RTC) {
                i2c_manager::DeviceHandle rtc_dev = {0x51, "RTC"};
                request.success = i2c_manager::read(rtc_dev, request.reg_addr,
                                                   request.data, request.data_len);
            }
            // Add other device types as needed
            break;

        case I2COperation::WRITE:
            if (request.device == I2CDeviceType::EXIO) {
                i2c_manager::DeviceHandle exio_dev = {0x20, "EXIO"};
                request.success = i2c_manager::write(exio_dev, request.reg_addr,
                                                    request.data, request.data_len);
            }
            // Add other device types as needed
            break;

        case I2COperation::PING:
            {
                i2c_manager::DeviceHandle dev = {request.device_addr, "PING"};
                request.success = i2c_manager::ping(dev);
            }
            break;

        case I2COperation::RTC_TIME_SET:
            // GPS one-shot time sync - write 7 bytes to RTC time registers
            if (request.device == I2CDeviceType::RTC && request.data_len == 7) {
                request.success = i2c_manager::write(i2c_manager::RTC_DEVICE, request.reg_addr,
                                                     request.data, request.data_len);
            }
            break;
    }

    if (!request.success) {
        system_stats.failed_i2c_requests++;
    }
}

static void processUIUpdate(const UIUpdate& update) {
    ui_manager::UIState& ui = ui_manager::getUIState();

    switch (update.type) {
        case UIUpdateType::STATUS_LABEL:
        case UIUpdateType::SENSOR_DATA:
        case UIUpdateType::NETWORK_INFO:
        case UIUpdateType::MEMORY_STATS:
            // Radar project: No status labels, all removed
            break;

        case UIUpdateType::RADAR_REFRESH:
            // Update radar display with current GPS position
            navigation::updateRadarDisplay();
            break;

        case UIUpdateType::ZOOM_CHANGE:
            // Thread-safe zoom change from button callback (queued)
            Serial.println("[UI_UPDATE] Processing zoom change (forward)");
            ui.cycleZoom();
            updateZoomDependentFeatures();  // Beacon proximity at 50m zoom
            navigation::updateRadarDisplay();
            break;

        case UIUpdateType::ZOOM_CHANGE_REVERSE:
            // Thread-safe zoom change from button callback (queued)
            Serial.println("[UI_UPDATE] Processing zoom change (reverse)");
            ui.cycleZoomReverse();
            updateZoomDependentFeatures();  // Beacon proximity at 50m zoom
            navigation::updateRadarDisplay();
            break;

        case UIUpdateType::SETTINGS_SCREEN:
            // Thread-safe settings screen navigation from button callback
            Serial.println("[UI_UPDATE] Processing settings screen request");
            navigation::goToSettingsScreen();
            break;

        case UIUpdateType::ENTER_STANDBY:
            // Thread-safe standby entry from button callback
            // CRITICAL: Called within display_mutex - safe to use LVGL
            Serial.println("[UI_UPDATE] Processing enter standby request");
            // Switch to 100m zoom immediately to stop beacon proximity beeping
            ui.resetZoom();
            updateZoomDependentFeatures();
            standby_manager::enterStandby();
            break;

        case UIUpdateType::WAKE_STANDBY:
            // Thread-safe wake from standby from button callback
            // CRITICAL: Called within display_mutex - safe to use LVGL
            Serial.println("[UI_UPDATE] Processing wake from standby request");
            standby_manager::wakeFromStandby();
            break;

        case UIUpdateType::BATTERY_UPDATE:
            // Thread-safe battery label update (fixes System Task race condition!)
            // CRITICAL: Called within display_mutex - safe to use LVGL
            {
                if (ui.battery_label && lv_obj_is_valid(ui.battery_label)) {
                    if (update.battery_percent < 0) {
                        // Invalid percentage
                        lv_label_set_text(ui.battery_label, LV_SYMBOL_BATTERY_EMPTY);
                    } else {
                        // Dev mode: percentage + voltage, Regular mode: battery symbol
                        if (settings_manager::getSettings().dev_mode) {
                            int v_int = (int)update.battery_voltage;
                            int v_dec = (int)((update.battery_voltage - v_int) * 100);
                            lv_label_set_text_fmt(ui.battery_label, "%d%%\n%d.%02dV", update.battery_percent, v_int, v_dec);
                        } else if (update.battery_voltage >= 4.20f) {
                            // Charging detected (USB connected, voltage above battery max)
                            lv_label_set_text(ui.battery_label, LV_SYMBOL_CHARGE);
                        } else {
                            // Battery symbol based on percentage
                            const char* bat_symbol;
                            if (update.battery_percent > 87)      bat_symbol = LV_SYMBOL_BATTERY_FULL;
                            else if (update.battery_percent > 62)  bat_symbol = LV_SYMBOL_BATTERY_3;
                            else if (update.battery_percent > 37)  bat_symbol = LV_SYMBOL_BATTERY_2;
                            else if (update.battery_percent > 12)  bat_symbol = LV_SYMBOL_BATTERY_1;
                            else                                   bat_symbol = LV_SYMBOL_BATTERY_EMPTY;
                            lv_label_set_text(ui.battery_label, bat_symbol);
                        }

                        // Color: black in daylight, white otherwise
                        lv_obj_set_style_text_color(ui.battery_label,
                            update.daylight_mode ? lv_color_black() : lv_color_white(), 0);
                    }
                }
            }
            break;

        case UIUpdateType::LOAD_RADAR_SCREEN:
            // Transition from loading screen to radar screen
            // CRITICAL: Called within display_mutex by UI Task - safe to use LVGL
            if (ui.screen_radar) {
                lv_scr_load(ui.screen_radar);
                // Show WiFi mode overlay if WiFi is active (radar disabled)
                if (ui.wifi_mode_label) {
                    if (settings_manager::getSettings().wifi_enabled) {
                        lv_obj_clear_flag(ui.wifi_mode_label, LV_OBJ_FLAG_HIDDEN);
                    } else {
                        lv_obj_add_flag(ui.wifi_mode_label, LV_OBJ_FLAG_HIDDEN);
                    }
                }
                Serial.println("[UI_TASK] Radar screen loaded via queue");
            }
            break;

        case UIUpdateType::LOAD_AP_SCREEN:
            // Transition from loading screen to AP upload mode screen (AP boot only)
            if (ui.screen_ap) {
                lv_scr_load(ui.screen_ap);
                Serial.println("[UI_TASK] AP upload screen loaded");
            }
            break;

        case UIUpdateType::LOAD_WIFI_SCREEN:
            // Transition from loading screen to WiFi STA mode screen (WiFi boot only)
            if (ui.screen_wifi) {
                lv_scr_load(ui.screen_wifi);
                Serial.println("[UI_TASK] WiFi STA screen loaded");
            }
            break;

        case UIUpdateType::BEACON_DBM_UPDATE: {
            if (ui.beacon_dbm_label && lv_obj_is_valid(ui.beacon_dbm_label)) {
                bool at_50m = (ui.current_zoom == ui_manager::ZoomLevel::ZOOM_50M);
                bool dev_mode = settings_manager::getSettings().dev_mode;
                if (at_50m && dev_mode && update.data[0] != '\0') {
                    lv_label_set_text(ui.beacon_dbm_label, update.data);
                    lv_color_t dbm_color = update.daylight_mode
                        ? lv_color_black()
                        : lv_color_white();
                    lv_obj_set_style_text_color(ui.beacon_dbm_label, dbm_color, 0);
                    lv_obj_clear_flag(ui.beacon_dbm_label, LV_OBJ_FLAG_HIDDEN);
                } else {
                    lv_obj_add_flag(ui.beacon_dbm_label, LV_OBJ_FLAG_HIDDEN);
                }
            }
            break;
        }

        case UIUpdateType::COMPASS_UPDATE: {
            float prev_heading = ui.current_heading;
            ui.current_heading = navigation::smoothHeading(
                ui.current_heading, update.compass_heading, navigation::HEADING_SMOOTHING);

            // Skip redraw if smoothed heading barely moved (< 1.5°).
            // Stationary compass noise produces sub-degree changes every second —
            // skipping those cuts Core 1 render load and reduces button poll gaps (FT-01).
            float delta = fabsf(ui.current_heading - prev_heading);
            if (delta > 180.0f) delta = 360.0f - delta;
            if (delta < 1.5f) break;

            // Only render if GPS has no valid fix.
            // When GPS has a fix, RADAR_REFRESH is queued in the same burst and
            // renders with this updated heading — rendering twice wastes 50ms of
            // UI Task time per second and causes long button polling gaps.
            if (!device_manager::getDeviceState().last_gps_data.valid) {
                navigation::updateRadarDisplay();
            }
            break;
        }

        case UIUpdateType::DEV_MODE_CHANGE: {
            // Show or hide the DEV label based on current dev_mode setting
            bool dev_on = settings_manager::getSettings().dev_mode;
            if (ui.log_indicator && lv_obj_is_valid(ui.log_indicator)) {
                if (dev_on) {
                    lv_obj_clear_flag(ui.log_indicator, LV_OBJ_FLAG_HIDDEN);
                } else {
                    lv_obj_add_flag(ui.log_indicator, LV_OBJ_FLAG_HIDDEN);
                }
            }
            break;
        }
    }
}

static void updateMemoryStats() {
    // Basic memory monitoring without full memory manager
    uint32_t heap_free = esp_get_free_heap_size();
    uint32_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    system_stats.memory_usage_bytes = heap_free + psram_free;
}

/**
 * @brief Attempt to recover a hung task via suspend/resume
 * @param task_handle FreeRTOS task handle
 * @param task_name Name for logging
 * @param health Task health tracking structure
 * @return true if recovery was attempted
 */
static bool attemptTaskRecovery(TaskHandle_t task_handle, const char* task_name, TaskHealth& health) {
    if (!task_handle) {
        return false;
    }

    // Check if we've exceeded max recovery attempts
    if (health.recovery_attempts >= TaskHealth::MAX_RECOVERY_ATTEMPTS) {
        Serial.printf("[HEALTH] %s: Max recovery attempts (%lu) reached - giving up\n",
                     task_name, health.recovery_attempts);

        // Log detailed failure info for post-crash analysis
        char fail_msg[192];
        snprintf(fail_msg, sizeof(fail_msg),
                 "%s UNRECOVERABLE: attempts=%lu heap=%lu uptime=%lums",
                 task_name, health.recovery_attempts,
                 esp_get_free_heap_size(), millis());
        system_logger::error("TASK_HEALTH", fail_msg);

        // Log stack high water mark for the failed task
        UBaseType_t stack_hwm = uxTaskGetStackHighWaterMark(task_handle);
        snprintf(fail_msg, sizeof(fail_msg),
                 "%s stack remaining: %u bytes (low=danger)",
                 task_name, (unsigned int)(stack_hwm * sizeof(StackType_t)));
        system_logger::error("TASK_HEALTH", fail_msg);

        // Capture system state for crash analysis

        return false;
    }

    health.recovery_attempts++;

    Serial.printf("[HEALTH] 🔧 Attempting recovery #%lu for %s...\n",
                 health.recovery_attempts, task_name);

    // Log recovery attempt with context
    char recovery_msg[128];
    snprintf(recovery_msg, sizeof(recovery_msg),
             "Recovery attempt #%lu for %s (heap=%lu)",
             health.recovery_attempts, task_name, esp_get_free_heap_size());
    system_logger::warn("TASK_HEALTH", recovery_msg);

    // Strategy: Suspend then resume the task
    // This can help if the task is blocked on a resource
    vTaskSuspend(task_handle);
    health.is_suspended = true;

    // Brief delay to let resources release
    vTaskDelay(pdMS_TO_TICKS(100));

    // Resume the task
    vTaskResume(task_handle);
    health.is_suspended = false;

    // Reset unresponsive count to give the task a chance
    health.unresponsive_count = 0;
    health.last_loop_time_ms = millis();

    Serial.printf("[HEALTH] ✓ %s resumed (attempt #%lu)\n", task_name, health.recovery_attempts);

    return true;
}

/**
 * @brief Check health of a specific task and attempt recovery if needed
 * @param task_handle FreeRTOS task handle
 * @param task_name Name for logging
 * @param stats Task statistics structure
 * @param last_loop_count Previous loop count for comparison
 * @param task_enabled Whether task is enabled
 * @return New loop count to track
 */
static uint32_t checkSingleTaskHealth(
    TaskHandle_t task_handle,
    const char* task_name,
    TaskStats& stats,
    uint32_t last_loop_count,
    bool task_enabled
) {
    if (!task_enabled || !task_handle) {
        stats.is_healthy = (task_handle != nullptr);
        return stats.loop_count;
    }

    uint32_t now = millis();

    // Check if task is making progress
    if (stats.loop_count == last_loop_count) {
        // Task hasn't made progress
        stats.health.unresponsive_count++;

        if (stats.health.unresponsive_count == 1) {
            // First detection - log warning
            Serial.printf("[HEALTH] ⚠️ %s not responding (loop count: %lu)\n",
                         task_name, last_loop_count);
            stats.is_healthy = false;
            strcpy(stats.status_message, "Unresponsive");
        } else if (stats.health.unresponsive_count >= 2) {
            // Multiple failures - attempt recovery
            Serial.printf("[HEALTH] 🔴 %s hung for %lu checks - attempting recovery\n",
                         task_name, stats.health.unresponsive_count);

            attemptTaskRecovery(task_handle, task_name, stats.health);
        }
    } else {
        // Task is making progress
        if (stats.health.unresponsive_count > 0) {
            // Task recovered
            Serial.printf("[HEALTH] ✓ %s recovered (was unresponsive for %lu checks)\n",
                         task_name, stats.health.unresponsive_count);
        }

        stats.health.unresponsive_count = 0;
        stats.health.last_loop_time_ms = now;
        stats.is_healthy = true;
        strcpy(stats.status_message, "Running");
    }

    return stats.loop_count;
}

static void checkTaskHealth() {
    // Check if all tasks are responding
    system_stats.tasks_running = tasks_running;
    system_stats.uptime_ms = millis();

    // Track previous loop counts for comparison
    static uint32_t last_ui_loops = 0;
    static uint32_t last_i2c_loops = 0;
    static uint32_t last_network_loops = 0;
    static uint32_t last_health_check = 0;

    uint32_t now = millis();

    // Health check interval: 5 seconds (more responsive than 30s)
    if (now - last_health_check >= 5000) {
        last_health_check = now;

        // Check each task's health with recovery support
        last_ui_loops = checkSingleTaskHealth(
            ui_task_handle, "UI_Task",
            system_stats.ui_task, last_ui_loops, task_enabled[0]);

        last_i2c_loops = checkSingleTaskHealth(
            i2c_task_handle, "I2C_Task",
            system_stats.i2c_task, last_i2c_loops, task_enabled[1]);

        last_network_loops = checkSingleTaskHealth(
            network_task_handle, "NET_Task",
            system_stats.network_task, last_network_loops, task_enabled[2]);

        // Note: System task checks itself
        system_stats.system_task.is_healthy = (system_task_handle != nullptr);
        system_stats.system_task.health.last_loop_time_ms = now;
    }
}

// =============================================================================
// PUBLIC API IMPLEMENTATION
// =============================================================================

bool init() {
    Serial.println("[TASK] Initializing Task Manager...");

    // Create synchronization primitives
    i2c_request_queue = xQueueCreate(TaskConfig::I2C_QUEUE_SIZE, sizeof(I2CRequest));
    ui_update_queue = xQueueCreate(TaskConfig::UI_QUEUE_SIZE, sizeof(UIUpdate));
    display_mutex = xSemaphoreCreateMutex();
    i2c_mutex = xSemaphoreCreateMutex();
    ui_state_mutex = xSemaphoreCreateMutex();

    if (!i2c_request_queue || !ui_update_queue || !display_mutex || !i2c_mutex || !ui_state_mutex) {
        Serial.println("[TASK] Failed to create synchronization primitives");
        return false;
    }

    // Initialize statistics
    memset(&system_stats, 0, sizeof(system_stats));

    Serial.println("[TASK] Task Manager initialized successfully");
    return true;
}

bool startTasks() {
    if (tasks_running) {
        Serial.println("[TASK] Tasks already running");
        return true;
    }

    Serial.println("[TASK] Starting FreeRTOS tasks...");

    // Set flag before creating tasks to avoid race condition
    tasks_running = true;

    const auto& boot_settings = settings_manager::getSettings();
    // AP boot skips I2C + Network + System — softAP beacon buffers + httpd leave ~8KB
    // internal SRAM, not enough for two more task stacks. Upload screen needs none of them:
    // no compass, no RTC display, no buzzer feedback.
    // WiFi STA boot skips only Network + System — upload UI is heavier, I2C still needed.
    bool ap_boot      = boot_settings.wifi_ap_enabled;
    bool minimal_boot = ap_boot || boot_settings.wifi_sta_boot;

    // Create UI Task (Core 1, Highest Priority) — always needed
    BaseType_t ui_result = xTaskCreatePinnedToCore(
        uiTask, "UI_Task", TaskConfig::UI_STACK_SIZE, nullptr,
        TaskConfig::UI_PRIORITY, &ui_task_handle, TaskConfig::UI_CORE
    );

    // Create I2C Task (Core 0, Medium Priority) — skip in AP boot to conserve SRAM
    BaseType_t i2c_result = pdPASS;
    if (!ap_boot) {
        i2c_result = xTaskCreatePinnedToCore(
            i2cTask, "I2C_Task", TaskConfig::I2C_STACK_SIZE, nullptr,
            TaskConfig::I2C_PRIORITY, &i2c_task_handle, TaskConfig::OTHER_CORE
        );
    }

    BaseType_t net_result = pdPASS;
    BaseType_t sys_result = pdPASS;

    if (!minimal_boot) {
        // Create Network Task (Core 0, Low Priority)
        net_result = xTaskCreatePinnedToCore(
            networkTask, "NET_Task", TaskConfig::NETWORK_STACK_SIZE, nullptr,
            TaskConfig::NETWORK_PRIORITY, &network_task_handle, TaskConfig::OTHER_CORE
        );

        // Create System Task (Core 0, Low Priority)
        sys_result = xTaskCreatePinnedToCore(
            systemTask, "SYS_Task", TaskConfig::SYSTEM_STACK_SIZE, nullptr,
            TaskConfig::SYSTEM_PRIORITY, &system_task_handle, TaskConfig::OTHER_CORE
        );
    }

    if (ap_boot) {
        Serial.println("[TASK] AP boot: only UI Task started (I2C/Network/System skipped)");
    } else if (minimal_boot) {
        Serial.println("[TASK] WiFi STA boot: skipping Network and System tasks");
    }

    if (ui_result != pdPASS || i2c_result != pdPASS ||
        net_result != pdPASS || sys_result != pdPASS) {
        Serial.println("[TASK] Failed to create one or more tasks");
        stopTasks();
        return false;
    }

    Serial.println("[TASK] All tasks started successfully");
    return true;
}

void stopTasks() {
    if (!tasks_running) {
        return;
    }

    Serial.println("[TASK] Stopping all tasks...");
    tasks_running = false;

    // Tasks call vTaskDelete(nullptr) themselves when they see tasks_running=false.
    // Wait for them to self-delete, then clear handles. Do NOT call vTaskDelete
    // externally — double-deleting a task TCB causes LoadProhibited panic.
    vTaskDelay(pdMS_TO_TICKS(300));
    ui_task_handle = nullptr;
    i2c_task_handle = nullptr;
    network_task_handle = nullptr;
    system_task_handle = nullptr;

    Serial.println("[TASK] All tasks stopped");
}

bool isTaskModeActive() {
    return tasks_running;
}

bool isSystemStable() {
    // If not using task architecture, system is always "stable" (legacy mode)
    if (!tasks_running) {
        return true;
    }

    // Check all enabled tasks are healthy and have completed at least 5 loops
    // This indicates FreeRTOS scheduler has stabilized and tasks are running properly

    bool all_healthy = true;
    bool all_running = true;

    // UI Task (always enabled)
    if (task_enabled[0]) {
        all_healthy &= system_stats.ui_task.is_healthy;
        all_running &= (system_stats.ui_task.loop_count >= 5);
    }

    // I2C Task (always enabled)
    if (task_enabled[1]) {
        all_healthy &= system_stats.i2c_task.is_healthy;
        all_running &= (system_stats.i2c_task.loop_count >= 5);
    }

    // Network Task (check if enabled)
    if (task_enabled[2]) {
        all_healthy &= system_stats.network_task.is_healthy;
        all_running &= (system_stats.network_task.loop_count >= 5);
    }

    // System Task (always enabled)
    if (task_enabled[3]) {
        all_healthy &= system_stats.system_task.is_healthy;
        all_running &= (system_stats.system_task.loop_count >= 5);
    }

    return all_healthy && all_running;
}

bool queueI2CRequest(const I2CRequest& req) {
    if (!i2c_request_queue) return false;
    return xQueueSend(i2c_request_queue, &req, pdMS_TO_TICKS(10)) == pdTRUE;
}

bool queueUIUpdate(const UIUpdate& update) {
    if (!ui_update_queue) return false;

    bool success = xQueueSend(ui_update_queue, &update, pdMS_TO_TICKS(5)) == pdTRUE;
    return success;
}

SystemStats getSystemStats() {
    return system_stats;
}

void setTasksEnabled(bool ui_enabled, bool i2c_enabled, bool network_enabled, bool system_enabled) {
    task_enabled[0] = ui_enabled;
    task_enabled[1] = i2c_enabled;
    task_enabled[2] = network_enabled;
    task_enabled[3] = system_enabled;

    Serial.printf("[TASK] Task enables: UI=%d I2C=%d NET=%d SYS=%d\n",
                  ui_enabled, i2c_enabled, network_enabled, system_enabled);
}

void printTaskStatus() {
    Serial.println("==== Task Manager Status ====");
    Serial.printf("Tasks Running: %s\n", tasks_running ? "YES" : "NO");
    Serial.printf("UI Task:  loops=%lu runtime=%lums stack=%lu health=%s\n",
                  system_stats.ui_task.loop_count, system_stats.ui_task.last_runtime_ms,
                  system_stats.ui_task.stack_high_water,
                  system_stats.ui_task.is_healthy ? "OK" : "FAIL");
    Serial.printf("I2C Task: loops=%lu runtime=%lums stack=%lu health=%s\n",
                  system_stats.i2c_task.loop_count, system_stats.i2c_task.last_runtime_ms,
                  system_stats.i2c_task.stack_high_water,
                  system_stats.i2c_task.is_healthy ? "OK" : "FAIL");
    Serial.printf("NET Task: loops=%lu runtime=%lums stack=%lu health=%s\n",
                  system_stats.network_task.loop_count, system_stats.network_task.last_runtime_ms,
                  system_stats.network_task.stack_high_water,
                  system_stats.network_task.is_healthy ? "OK" : "FAIL");
    Serial.printf("SYS Task: loops=%lu runtime=%lums stack=%lu health=%s\n",
                  system_stats.system_task.loop_count, system_stats.system_task.last_runtime_ms,
                  system_stats.system_task.stack_high_water,
                  system_stats.system_task.is_healthy ? "OK" : "FAIL");
    Serial.printf("I2C Requests: total=%lu failed=%lu\n",
                  system_stats.total_i2c_requests, system_stats.failed_i2c_requests);
    Serial.printf("Uptime: %lums\n", system_stats.uptime_ms);

    // GPS Health Metrics (Tier 1 Priority)
    Serial.println();
    Serial.println("==== GPS Health (TIER 1) ====");
    const device_manager::DeviceState& dev_state = device_manager::getDeviceState();
    if (dev_state.gps_ok) {
        Serial.printf("GPS Status: %s\n", dev_state.last_gps_data.valid ? "FIXED" : "SEARCHING");
        Serial.printf("Satellites: %d\n", dev_state.last_gps_data.sats);
        if (dev_state.last_gps_data.valid) {
            Serial.printf("Position: %.6f, %.6f\n", dev_state.last_gps_data.lat, dev_state.last_gps_data.lon);
            Serial.printf("Altitude: %.1fm\n", dev_state.last_gps_data.alt);
            Serial.printf("HDOP: %.1f\n", dev_state.last_gps_data.hdop);
            Serial.printf("Speed: %.1f km/h\n", dev_state.last_gps_data.speed);
            Serial.printf("Course: %.1f°\n", dev_state.last_gps_data.course);
        }
        uint32_t age_ms = millis() - dev_state.last_gps_data.last_update_ms;
        Serial.printf("Last Update: %lu ms ago\n", age_ms);
        if (age_ms > 5000) {
            Serial.println("⚠️  WARNING: GPS data is stale (>5s old)");
        }
    } else {
        Serial.println("GPS Status: NOT INITIALIZED");
    }

    Serial.println("==============================");
}

/**
 * @brief GPS/compass/battery update — called every 1s by System Task
 */
static void updateStatusLabels() {
    // Radar project: GPS reading and serial output
    static GPSData s_last_gps_data;
    static uint32_t s_last_gps_read = 0;


    uint32_t now = millis();
    if (now - s_last_gps_read >= system_config::timing::GPS_UPDATE_INTERVAL_MS) {
        s_last_gps_read = now;

        // Read GPS data
        GPSData gps_data;
        if (gps_bh880::read(gps_data)) {
            // Update timestamp for quality tracking (Phase 1.3)
            gps_data.last_update_ms = millis();

            s_last_gps_data = gps_data;

            // GPS Quality Tracking (Phase 1.3)
            // CRITICAL: Check for position jumps BEFORE storing GPS data
            // Position jumps indicate corrupted NMEA data (e.g., lon=11802 instead of -118)
            gps_quality::QualityMetrics quality = gps_quality::update(gps_data);

            // If position jump detected, skip this GPS update entirely
            // This prevents waypoints from disappearing due to corrupted coordinates
            bool gps_position_valid = quality.position_stable || quality.updates_received <= 1;

            // HDOP gate: reject positions with extremely poor accuracy (module is confused)
            // First fix (updates_received <= 1) is always allowed to establish initial position
            constexpr float MAX_HDOP_ACCEPT = 10.0f;
            bool hdop_acceptable = isnan(gps_data.hdop) || gps_data.hdop <= MAX_HDOP_ACCEPT;
            gps_position_valid = gps_position_valid && (quality.updates_received <= 1 || hdop_acceptable);
            if (!gps_position_valid) {
                if (!quality.position_stable) {
                    Serial.println("[GPS] REJECTED: Position jump detected, keeping last good position");
                } else {
                    Serial.printf("[GPS] REJECTED: HDOP=%.1f (>%.0f), keeping last good position\n",
                                  gps_data.hdop, MAX_HDOP_ACCEPT);
                }
            }

            // Only store and use GPS data if position is valid (no jump detected)
            if (gps_position_valid) {
                // Store in device state for diagnostic access
                // NOTE: GPS data is updated atomically (single assignment) - no mutex needed
                // Using display_mutex here was causing UI Task starvation (display_mutex held 300ms+)
                // which led to mutex timeouts and race conditions
                device_manager::DeviceState& dev_state = const_cast<device_manager::DeviceState&>(device_manager::getDeviceState());
                dev_state.last_gps_data = gps_data;
            }

            // ===================================================================
            // GPS STATE SAVING - For Hot/Warm Start on next boot
            // ===================================================================
            // Save position every 5 minutes when we have a valid fix
            // CRITICAL: Only save if position is valid (no jump detected)
            // This enables hot start (<4h) or warm start (<24h) on next boot
            static uint32_t s_last_gps_save = 0;
            constexpr uint32_t GPS_SAVE_INTERVAL_MS = 300000;  // 5 minutes

            if (gps_position_valid && gps_data.valid && (now - s_last_gps_save >= GPS_SAVE_INTERVAL_MS)) {
                s_last_gps_save = now;

                // Convert GPS time to Unix timestamp
                if (gps_data.hasTime && gps_data.year > 2020) {
                    struct tm tm_gps = {};
                    tm_gps.tm_year = gps_data.year - 1900;
                    tm_gps.tm_mon = gps_data.month - 1;
                    tm_gps.tm_mday = gps_data.day;
                    tm_gps.tm_hour = gps_data.hour;
                    tm_gps.tm_min = gps_data.minute;
                    tm_gps.tm_sec = gps_data.second;
                    uint32_t fix_time = mktime(&tm_gps);

                    // Save to NVS for hot/warm start on next boot
                    if (settings_manager::saveGPSState(gps_data.lat, gps_data.lon, fix_time)) {
                        Serial.printf("[GPS_STATE] ✓ Position saved for fast fix: %.6f, %.6f\n",
                                     gps_data.lat, gps_data.lon);
                    }
                }
            }

            // GPS TIME SYNC - ONE-SHOT VIA I2C QUEUE
            // Uses task_manager I2C queue to avoid bus contention with GPS UART
            if (gps_position_valid && gps_data.valid && gps_data.hasTime &&
                !ntp_sync::isGPSTimeSynced()) {
                ntp_sync::queueGPSTimeSync(gps_data);
            }

            // ===================================================================
            // MAGNETIC DECLINATION - WMM computation at first GPS fix
            // ===================================================================
            // Computed once per session (or when position changes >100km).
            // Result cached in NVS and applied to every compass reading.
            // Requires GPS position + time to form decimal year.
            {
                const auto& decl_cfg = settings_manager::getSettings();
                static double s_decl_lat = 999.0;  // Last lat used for WMM
                static double s_decl_lon = 999.0;  // Last lon used for WMM

                bool need_compute = false;
                if (gps_position_valid && gps_data.valid && gps_data.hasTime &&
                    gps_data.year > 2020) {
                    if (!decl_cfg.compass_declination_valid) {
                        need_compute = true;  // First time — never computed
                    } else {
                        // Re-compute if moved >100km from last computation point
                        double dlat = gps_data.lat - s_decl_lat;
                        double dlon = gps_data.lon - s_decl_lon;
                        if (dlat*dlat + dlon*dlon > 0.81) {  // ~100km ≈ 0.9° lat/lon
                            need_compute = true;
                        }
                    }
                }

                if (need_compute) {
                    // Convert GPS time to decimal year
                    struct tm tm_wmm = {};
                    tm_wmm.tm_year  = gps_data.year - 1900;
                    tm_wmm.tm_mon   = gps_data.month - 1;
                    tm_wmm.tm_mday  = gps_data.day;
                    tm_wmm.tm_hour  = gps_data.hour;
                    tm_wmm.tm_min   = gps_data.minute;
                    tm_wmm.tm_sec   = gps_data.second;
                    uint32_t fix_unix = (uint32_t)mktime(&tm_wmm);
                    float decimal_year = wmm::toDecimalYear(fix_unix);

                    float decl = wmm::computeDeclination(
                        (float)gps_data.lat, (float)gps_data.lon,
                        gps_data.alt, decimal_year);

                    settings_manager::saveDeclination(decl);
                    s_decl_lat = gps_data.lat;
                    s_decl_lon = gps_data.lon;
                    Serial.printf("[WMM] Declination computed: %.2f° E at (%.4f, %.4f) %.1f\n",
                                  decl, gps_data.lat, gps_data.lon, decimal_year);
                }
            }

            // Print to serial every 30 seconds
            {
                static uint32_t s_gps_last_print_ms = 0;
                if (now - s_gps_last_print_ms >= 30000) {
                    s_gps_last_print_ms = now;
                    if (gps_data.valid) {
                        Serial.printf("GPS: Lat=%.6f, Lon=%.6f, Sats=%d, Alt=%.1fm, HDOP=%.1f%s\n",
                                      gps_data.lat, gps_data.lon, gps_data.sats,
                                      gps_data.alt, gps_data.hdop,
                                      gps_position_valid ? "" : " [REJECTED]");
                    } else {
                        Serial.printf("GPS: Searching... Sats=%d\n", gps_data.sats);
                    }
                }
            }

            // Update radar display if GPS has valid fix AND position is stable
            // CRITICAL: Skip radar update if position jump detected to prevent waypoint disappearance
            if (gps_position_valid && gps_data.valid) {
                UIUpdate radar_update;
                radar_update.type = UIUpdateType::RADAR_REFRESH;
                radar_update.timestamp = millis();  // Fix: Set timestamp to avoid bogus latency values
                queueUIUpdate(radar_update);
            }
        }
    }

    // =========================================================================
    // COMPASS UPDATE - Read QMC5883L magnetometer (~1Hz via System Task)
    // NOTE: Must stay in System Task, NOT I2C Task — see docs/compass_i2c_constraint.md
    //
    // WiFi AP isolation: compass reads are fully suspended while WiFi is enabled.
    // WiFi RF interrupts delay I2C bit timing → NACK → ESP_ERR_INVALID_STATE on the
    // shared bus, which also hosts the CST820 touch chip. Gating here prevents that
    // entirely. On WiFi close, compass is re-initialized so it resumes in a clean state.
    // =========================================================================
    {
        const device_manager::DeviceState& compass_dev_state = device_manager::getDeviceState();
        if (compass_dev_state.compass_ok) {
            static uint32_t s_last_compass_read = 0;
            static uint8_t  s_compass_fail_count = 0;
            static bool     s_wifi_was_active = false;
            static bool     s_was_in_standby = false;

            // AP mode starts WiFi directly (bypasses wifi_manager), so check both:
            // isEnabled() = STA mode on, gpx_server::isRunning() = AP mode on
            const bool wifi_active = wifi_manager::isEnabled() || gpx_server::isRunning();
            const bool in_standby  = standby_manager::isStandby();

            if (wifi_active) {
                // WiFi AP is up — suspend compass reads entirely.
                // No I2C traffic → no RF/I2C collisions → touch chip unaffected.
                s_wifi_was_active = true;

            } else if (in_standby) {
                // Standby — suspend reads to reduce I2C bus stress.
                // Hundreds of reads/hour over a long sleep gradually corrupt the bus;
                // suspending here prevents the TCA9554 from getting stuck mid-transaction.
                s_was_in_standby = true;

            } else if (s_wifi_was_active) {
                // WiFi just went down — re-initialize compass before resuming reads.
                // The chip may have accumulated errors during the WiFi session;
                // a clean reset guarantees it comes back in continuous-mode.
                s_wifi_was_active = false;
                s_compass_fail_count = 0;
                s_last_compass_read = millis();
                Serial.println("[COMPASS] WiFi off — reinitializing compass");
                compass_qmc5883l::reset();  // soft reset + full re-init

            } else if (s_was_in_standby) {
                // Just woke from standby — reinitialize compass chip registers.
                // The I2C bus was reinited by restoreActivePowerSettings(); now
                // reconfigure the QMC5883L to continuous-mode before resuming.
                //
                // CRITICAL: restoreActivePowerSettings() does several delay() calls
                // (delay(20) + delay(15) + delay(100) for CST820 reset) while running
                // in the UI Task. Each delay() yields via vTaskDelay, allowing the
                // System Task to slip in while the bus is still settling. Calling
                // reset() that early NACKs the soft-reset write → ESP_ERR_INVALID_STATE
                // spam + the 5-failure cycle. Wait 500ms to guarantee
                // restoreActivePowerSettings() has fully returned before we touch
                // the compass. 500ms >> the ~200ms total reinit+CST820 sequence.
                vTaskDelay(pdMS_TO_TICKS(500));
                s_was_in_standby = false;
                s_compass_fail_count = 0;
                s_last_compass_read = millis();
                Serial.println("[COMPASS] Standby wake — reinitializing compass");
                compass_qmc5883l::reset();  // soft reset + full re-init

            } else {
                // Normal operation — read compass
                uint32_t compass_now = millis();
                if (compass_now - s_last_compass_read >= 20) {
                    s_last_compass_read = compass_now;

                    CompassData compass_data;
                    if (compass_qmc5883l::read(compass_data)) {
                        s_compass_fail_count = 0;
                        device_manager::DeviceState& mutable_state = const_cast<device_manager::DeviceState&>(compass_dev_state);
                        mutable_state.last_compass_data = compass_data;

                        if (compass_data.valid) {
                            // Apply magnetic declination (WMM) to convert magnetic → true heading
                            float true_heading = compass_data.heading;
                            const auto& decl_settings = settings_manager::getSettings();
                            if (decl_settings.compass_declination_valid) {
                                true_heading += decl_settings.compass_declination_deg;
                                if (true_heading < 0.0f)    true_heading += 360.0f;
                                if (true_heading >= 360.0f) true_heading -= 360.0f;
                            }

                            UIUpdate upd = {};
                            upd.type = UIUpdateType::COMPASS_UPDATE;
                            upd.compass_heading = true_heading;
                            upd.timestamp = compass_now;
                            queueUIUpdate(upd);
                        }
                    } else {
                        s_compass_fail_count++;
                        if (s_compass_fail_count >= 5) {
                            Serial.println("[COMPASS] 5 consecutive failures — attempting soft reset");
                            s_compass_fail_count = 0;
                            compass_qmc5883l::reset();
                        }
                    }
                }
            }
        }
    }

    // =========================================================================
    // BATTERY UPDATE - Queue to UI Task (FIXES RACE CONDITION!)
    // =========================================================================
    // CRITICAL: Never call LVGL functions directly from System Task!
    // Throttled to 30s: battery symbol has only 5 states and changes slowly.
    // Queueing every 1s just adds churn to the UI queue with no visible benefit.
    static uint32_t battery_update_counter = 0;
    static uint32_t last_battery_queue_ms = 0;
    battery_update_counter++;

    if (now - last_battery_queue_ms < 30000) {
        return;  // Only queue battery update every 30 seconds
    }
    last_battery_queue_ms = now;

    battery::BatteryStatus bat_status = battery::getStatus();

    // Queue battery update for UI Task to process safely
    UIUpdate battery_update;
    battery_update.type = UIUpdateType::BATTERY_UPDATE;
    battery_update.timestamp = millis();
    battery_update.battery_percent = (bat_status.percent >= 0 && bat_status.percent <= 100)
                                      ? (int8_t)bat_status.percent
                                      : -1;  // -1 indicates invalid
    battery_update.battery_voltage = bat_status.voltage;

    // Get daylight mode from cached settings (no I/O)
    battery_update.daylight_mode = settings_manager::getSettings().daylight_mode;

    if (!queueUIUpdate(battery_update)) {
        // Queue full - rare, log for diagnostics
        if (battery_update_counter % 12 == 0) {
            Serial.println("[BAT_UPDATE] Queue full - skipped update");
        }
    }

    // Diagnostic logging: Every 60 seconds (12 * 5s intervals)
    if (battery_update_counter % 12 == 0) {
        Serial.printf("[BAT_UPDATE] Queued %d%% (%.2fV, %s) - Update #%lu\n",
                      bat_status.percent, bat_status.voltage,
                      battery::batteryStateToString(bat_status.battery_state),
                      battery_update_counter);
    }

    // Beacon dBm label — DEV mode + 50m zoom only (for field RSSI calibration)
    const auto& settings = settings_manager::getSettings();
    if (settings.dev_mode && settings.beacon_proximity_enabled) {
        const ui_manager::UIState& ui_state = ui_manager::getUIState();
        bool at_50m = (ui_state.current_zoom == ui_manager::ZoomLevel::ZOOM_50M);
        if (at_50m) {
            auto bs = beacon_proximity::getState();
            UIUpdate dbm_update{};
            dbm_update.type = UIUpdateType::BEACON_DBM_UPDATE;
            dbm_update.timestamp = millis();
            dbm_update.daylight_mode = settings.daylight_mode;
            snprintf(dbm_update.data, sizeof(dbm_update.data),
                     "%d dBm (raw)\n%.0f dBm (ema)\n%s",
                     (int)bs.rssi_raw,
                     bs.rssi_ema,
                     beacon_proximity::zoneToString(bs.zone));
            queueUIUpdate(dbm_update);
        }
    }
}

// =============================================================================
// THREAD-SAFE UI STATE ACCESSORS
// =============================================================================

int getCurrentZoomLevel() {
    int level = 0;
    if (ui_state_mutex && xSemaphoreTake(ui_state_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        level = static_cast<int>(ui_manager::getUIState().current_zoom);
        xSemaphoreGive(ui_state_mutex);
    }
    return level;
}

void cycleZoomForward() {
    if (ui_state_mutex && xSemaphoreTake(ui_state_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        ui_manager::getUIState().cycleZoom();
        updateZoomDependentFeatures();  // Beacon proximity at 50m zoom
        xSemaphoreGive(ui_state_mutex);
    }
}

void cycleZoomBackward() {
    if (ui_state_mutex && xSemaphoreTake(ui_state_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        ui_manager::getUIState().cycleZoomReverse();
        updateZoomDependentFeatures();  // Beacon proximity at 50m zoom
        xSemaphoreGive(ui_state_mutex);
    }
}

bool withDisplayMutex(void (*func)(), uint32_t timeout_ms) {
    if (!display_mutex || !func) {
        return false;
    }

    if (xSemaphoreTake(display_mutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE) {
        func();
        xSemaphoreGive(display_mutex);
        return true;
    }

    return false;
}

} // namespace task_manager