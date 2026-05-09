// Waveshare ESP32-S3 Touch LCD 2.1"
// GPS Radar — ESP-IDF entry point

#include "core/arduino_compat.h"
#include <lvgl.h>
#include "esp_ota_ops.h"

// Modular components
#include "core/device_manager.h"
#include "ui/ui_manager.h"
#include "ui/navigation.h"
#include "ui/settings_screen.h"
#include "utils/diagnostics.h"
#include "utils/task_manager.h"
#include "utils/system_logger.h"
#include "utils/ntp_sync.h"
#include "utils/standby_manager.h"
#include "core/system_config.h"
#include "wifi_manager.h"
#include "scanner.h"
#include "gpx/gpx_server.h"
#include "gpx/gpx_loader.h"
#include "settings_manager.h"
#include "gps_bh880.h"
#include "backlight.h"
#include "i2c_manager.h"
#include "hardware/sensors/battery.h"
#include "utils/watchdog.h"
#include "hardware/connectivity/beacon_proximity.h"

extern "C" void app_main() {
    // Serial is USB CDC — begin() is a no-op in ESP-IDF
    Serial.begin(115200);
    uint32_t t0 = millis();
    while (!Serial && (millis() - t0 < 1500)) { delay(10); }

    // BOOT PHASE 1: Serial initialized
    Serial.printf("[T+%08lu] BOOT: Phase 1 - Serial initialized\n", millis());

    Serial.println("==== GPS Radar - FreeRTOS Task Architecture ====");

    // BOOT PHASE 2: Device manager (all hardware)
    Serial.printf("[T+%08lu] BOOT: Phase 2 - Initializing hardware...\n", millis());
    device_manager::Config dev_config;
    if (!device_manager::initializeAll(dev_config)) {
        Serial.println("[ERROR] Device initialization failed");
        while (1) { delay(1000); }
    }
    Serial.printf("[T+%08lu] BOOT: Phase 2 - Hardware initialized\n", millis());

    // BOOT PHASE 3: System logger — deferred to after settings load.
    // In normal mode the logger is never initialized and uses zero SRAM.

    // BOOT PHASE 4: NTP time sync (GPS-based only — WiFi NTP removed)
    Serial.printf("[T+%08lu] BOOT: Phase 4 - Initializing time sync module...\n", millis());
    ntp_sync::init(false);

    // BOOT PHASE 5: Settings manager and GPS settings
    Serial.printf("[T+%08lu] BOOT: Phase 5 - Loading settings from NVS...\n", millis());
    if (!settings_manager::init()) {
        Serial.println("[WARN] Settings manager initialization failed - using defaults");
        system_logger::warn("BOOT", "Settings manager init failed - using defaults");
    } else {
        Serial.println("[SETTINGS] Settings manager initialized successfully");
        system_logger::info("BOOT", "Phase 5 - Settings loaded from NVS");

        settings_manager::RadarSettings settings;
        if (settings_manager::loadSettings(settings)) {
            Serial.println("[SETTINGS] Settings loaded from NVS");

            // Apply brightness setting
            if (settings.brightness > 0) {
                uint8_t brightness_percent = (settings.brightness * 100) / 255;
                backlight::setPercent(brightness_percent);
                Serial.printf("[DISPLAY] ✓ Brightness restored to %d%%\n", brightness_percent);
            }

            // WiFi is session-only — never auto-start at boot.
            // WiFi stack is allocated early (device_manager step 5) for heap layout,
            // but the radio stays off until the user enables it in settings.

            // System logger — only init when enabled (zero overhead in normal mode)
            if (settings.logging_enabled) {
                if (system_logger::init()) {
                    Serial.printf("[LOG] Logger ready: %s\n", system_logger::LOG_FILE);
                    system_logger::info("BOOT", "System logger started");
                } else {
                    Serial.println("[WARN] System logger init failed");
                }
            }
            if (settings.dev_mode) {
                Serial.println("[DEV] ==========================================");
                Serial.println("[DEV] DEV MODE ACTIVE at boot");
                Serial.println("[DEV] ==========================================");

                system_logger::info("DEV", "==========================================");
                system_logger::info("DEV", "DEV MODE VERBOSE LOGGING ENABLED");
                system_logger::info("DEV", "- UI Task checkpoints: every 100 loops");
                system_logger::info("DEV", "- Button events: logged to SD");
                system_logger::info("DEV", "- Queue operations: logged to SD");
                system_logger::info("DEV", "- Heartbeat interval: 30 seconds");
                system_logger::info("DEV", "==========================================");
            }

            // Apply WiFi AP mode boot
            if (settings.wifi_ap_enabled) {
                Serial.println("[WIFI_AP] AP boot mode — starting Access Point...");
                if (wifi_manager::init()) {
                    if (gpx_server::start()) {
                        Serial.println("[WIFI_AP] ✓ AP created and web server started");
                        char ip[16];
                        if (gpx_server::getStatus(ip, sizeof(ip))) {
                            Serial.printf("[WIFI_AP] Web portal ready at: http://%s\n", ip);
                        }
                    } else {
                        Serial.println("[WIFI_AP] ✗ Failed to start AP/web server");
                    }
                } else {
                    Serial.println("[WIFI_AP] ✗ WiFi stack init failed — AP not started");
                }
            }
            // Apply WiFi STA mode boot
            else if (settings.wifi_sta_boot) {
                Serial.println("[WIFI_STA] WiFi STA boot mode — starting WiFi...");
                if (wifi_manager::init()) {
                    scanner::init();  // Enable scan capability for the WiFi screen
                    wifi_manager::setEnabled(true);
                    if (wifi_manager::autoConnect()) {
                        Serial.println("[WIFI_STA] Auto-connect initiated (connecting to saved network)");
                    } else {
                        Serial.println("[WIFI_STA] No saved credentials — waiting for user to scan and connect");
                    }
                } else {
                    Serial.println("[WIFI_STA] ✗ WiFi stack init failed");
                }
            }
        } else {
            Serial.println("[SETTINGS] No saved settings found, using defaults");
        }
    }

    // BOOT PHASE 6: UI manager (LVGL interface)
    Serial.printf("[T+%08lu] BOOT: Phase 6 - Initializing UI manager...\n", millis());
    system_logger::info("BOOT", "Phase 6 - Initializing UI manager");
    ui_manager::Config ui_config;
    if (!ui_manager::init(ui_config)) {
        Serial.println("[ERROR] UI initialization failed");
        system_logger::error("BOOT", "UI manager initialization FAILED");
        while (1) { delay(1000); }
    }
    system_logger::info("BOOT", "Phase 6 - UI manager initialized");

    // Create and display loading screen
    Serial.println("[MAIN] Displaying loading screen...");
    ui_manager::createLoadingScreen();
    ui_manager::UIState& ui = ui_manager::getUIState();
    lv_scr_load(ui.screen_loading);
    device_manager::enableLVGLProcessing();
    lv_task_handler();
    Serial.println("[MAIN] Loading screen displayed");

    ui_manager::updateLoadingStatus("Warming up circuits...");
    delay(200);

    // Dedicated boot modes: create the appropriate full-screen UI
    {
        const auto& boot = settings_manager::getSettings();
        if (boot.wifi_ap_enabled) {
            ui_manager::updateLoadingStatus("Starting upload mode...");
            ui_manager::createAPScreen();
            Serial.println("[MAIN] AP upload screen created");
        } else if (boot.wifi_sta_boot) {
            ui_manager::updateLoadingStatus("Starting WiFi mode...");
            ui_manager::createWiFiScreen();
            Serial.println("[MAIN] WiFi STA screen created");
        }
    }

    // Pre-create settings screen (eliminates first-press delay)
    ui_manager::updateLoadingStatus("Building control panels...");
    Serial.println("[MAIN] Pre-creating settings screen...");
    uint32_t settings_start = millis();
    ui_manager::createSettingsScreen();
    Serial.printf("[MAIN] ✓ Settings screen ready (%.1fs)\n",
                  (millis() - settings_start) / 1000.0);

    // Initialize navigation system
    ui_manager::updateLoadingStatus("Calibrating compass...");
    if (!navigation::init()) {
        Serial.println("[ERROR] Navigation initialization failed");
        while (1) { delay(1000); }
    }

    // Initialize diagnostics
    ui_manager::updateLoadingStatus("Running diagnostics...");
    diagnostics::Config diag_config;
    if (!diagnostics::init(diag_config)) {
        Serial.println("[ERROR] Diagnostics initialization failed");
        while (1) { delay(1000); }
    }

    // Initialize standby manager
    if (!standby_manager::init()) {
        Serial.println("[WARN] Standby manager initialization failed");
    }

    Serial.println("[MAIN] Button initialized by device_manager");

    // WiFi auto-connect (non-blocking)
    // Skip in wifi_sta_boot mode — Phase 5 already called autoConnect().
    // A second call here disconnects the already-established session.
    ui_manager::updateLoadingStatus("Scanning for networks...");
    {
        const auto& ws = settings_manager::getSettings();
        if (!ws.wifi_sta_boot && wifi_manager::isEnabled()) {
            Serial.println("[WIFI] Attempting auto-connect to saved network...");
            if (wifi_manager::autoConnect()) {
                Serial.println("[WIFI] Auto-connect initiated (background connection)");
            } else {
                Serial.println("[WIFI] No saved credentials found");
            }
        } else if (ws.wifi_sta_boot) {
            Serial.println("[WIFI] STA boot mode — auto-connect already initiated in Phase 5");
        } else {
            Serial.println("[WIFI] Auto-connect skipped (WiFi disabled in settings)");
        }
    }

    // WiFi manager update timer — needed in both normal STA mode and STA boot mode
    if (settings_manager::getSettings().wifi_enabled ||
        settings_manager::getSettings().wifi_sta_boot) {
        auto wifi_update_callback = [](lv_timer_t* timer) {
            wifi_manager::update();
        };
        lv_timer_t* wifi_update_timer = lv_timer_create(wifi_update_callback, 1000, nullptr);
        if (wifi_update_timer) {
            Serial.println("[WIFI] WiFi manager update timer started");
        } else {
            Serial.println("[WARN] Failed to create WiFi manager update timer");
        }
    } else {
        Serial.println("[WIFI] WiFi disabled — skipping WiFi update timers");
    }

    // WiFi UI status update timer (every 2 seconds)
    auto wifi_ui_callback = [](lv_timer_t* timer) {
        settings_screen::updateWiFiStatus();
        settings_screen::updateAPModeStatus();
    };
    lv_timer_t* wifi_ui_timer = lv_timer_create(wifi_ui_callback, 2000, nullptr);
    if (wifi_ui_timer) {
        Serial.println("[WIFI] WiFi UI update timer started successfully");
    } else {
        Serial.println("[WARN] Failed to create WiFi UI update timer");
    }

    // Initialize GPX web server
    Serial.println("[GPX] Initializing GPX web server...");
    if (!gpx_server::init()) {
        Serial.println("[WARN] GPX server initialization failed (SD card may not be available)");
    } else {
        Serial.println("[GPX] GPX server initialized successfully");
    }

    // Auto-load GPX files from SD card
    ui_manager::updateLoadingStatus("Loading waypoints...");
    Serial.println("[GPX] Auto-loading waypoints from /sdcard/gpx/ folder...");
    int waypoints_loaded = gpx_loader::loadAllGPXFiles();
    if (waypoints_loaded > 0) {
        navigation::updateRadarDisplay();
    } else {
        Serial.println("[GPX] No waypoints loaded (folder empty or SD unavailable)");
    }

    // Initialize Task Watchdog Timer
    ui_manager::updateLoadingStatus("Arming watchdog...");
    Serial.println("[WATCHDOG] Initializing Task Watchdog Timer...");
    watchdog::Config wdt_config;
    wdt_config.timeout_seconds = 30;
    wdt_config.panic_on_timeout = false;
    wdt_config.enable_idle_task = false;

    if (!watchdog::init(wdt_config)) {
        Serial.println("[WARN] Watchdog initialization failed");
    } else {
        Serial.println("[WATCHDOG] TWDT initialized with 30s timeout");
    }

    // Boot messages — declared here so they're in scope for both the pre-task
    // UI update (safe LVGL call) and the post-task serial loop.
    const char* boot_messages[] = {
        "Aligning satellites...",
        "Charging flux capacitor...",
        "Consulting the oracle...",
        "Downloading more RAM...",
        "Feeding the hamsters...",
        "Polishing pixels...",
        "Reticulating splines...",
        "Syncing with the cloud...",
        "Warming up the radar...",
        "Calibrating sensors..."
    };
    const int num_messages = sizeof(boot_messages) / sizeof(boot_messages[0]);
    randomSeed(millis());
    int idx = random(num_messages);

    // BOOT PHASE 9: Task manager (FreeRTOS architecture)
    // updateLoadingStatus is safe here — UI Task not started yet, LVGL is single-threaded.
    ui_manager::updateLoadingStatus(boot_messages[idx]);
    Serial.printf("[T+%08lu] BOOT: Phase 9 - Initializing task manager...\n", millis());
    system_logger::info("BOOT", "Phase 9 - Initializing FreeRTOS tasks");
    if (!task_manager::init()) {
        Serial.println("[ERROR] Task manager initialization failed");
        system_logger::error("BOOT", "Task manager init FAILED");
        while (1) { delay(1000); }
    }
    Serial.println("[TASK] Task manager initialized successfully");
    system_logger::info("BOOT", "Task manager queues and mutexes created");

    // BLE init: only in radar boot (no WiFi radio active).
    // AP boot and WiFi STA boot both use the WiFi stack — NimBLE must stay off
    // to avoid DMA SRAM fragmentation that breaks WPA2 AES cipher allocation.
    {
        const auto& bs = settings_manager::getSettings();
        if (!bs.wifi_ap_enabled && !bs.wifi_sta_boot) {
            beacon_proximity::init();
        }
    }

    if (!task_manager::startTasks()) {
        Serial.println("[ERROR] Failed to start tasks");
        system_logger::error("BOOT", "FreeRTOS tasks start FAILED");
        while (1) { delay(1000); }
    }
    Serial.println("[TASK] FreeRTOS tasks started successfully");
    system_logger::info("BOOT", "Phase 9 - All FreeRTOS tasks started");

    Serial.println("==== Initialization Complete ====");
    Serial.println("Running in FreeRTOS task mode - improved responsiveness");
    Serial.println("Type 'task status' to view task information");
    Serial.println("Type 'help' for available commands");

    // Wait for FreeRTOS tasks to stabilize before showing radar screen.
    // UI Task owns LVGL after startTasks() — do NOT call updateLoadingStatus here.
    // Serial keeps rotating messages; UI shows the one message set above.
    Serial.println("[MAIN] Waiting for system stabilization...");
    uint32_t init_start = millis();
    uint32_t last_message = 0;
    int message_count = 0;

    Serial.printf("[MAIN] Boot message %d: %s\n", ++message_count, boot_messages[idx]);

    while (millis() - init_start < 5000) {
        delay(100);
        if (message_count < 3 && (millis() - last_message >= 2000)) {
            idx = random(num_messages);
            last_message = millis();
            Serial.printf("[MAIN] Boot message %d: %s\n", ++message_count, boot_messages[idx]);
        }
    }

    Serial.printf("[MAIN] Boot wait complete after %.1fs, transitioning to radar screen...\n",
                 (millis() - init_start) / 1000.0);

    // Queue screen transition via UI Task
    {
        const auto& final_settings = settings_manager::getSettings();
        task_manager::UIUpdate screen_update = {};
        const char* screen_name;
        if (final_settings.wifi_ap_enabled) {
            screen_update.type = task_manager::UIUpdateType::LOAD_AP_SCREEN;
            screen_name = "AP upload";
        } else if (final_settings.wifi_sta_boot) {
            screen_update.type = task_manager::UIUpdateType::LOAD_WIFI_SCREEN;
            screen_name = "WiFi STA";
        } else {
            screen_update.type = task_manager::UIUpdateType::LOAD_RADAR_SCREEN;
            screen_name = "Radar";
        }
        screen_update.timestamp = millis();
        if (task_manager::queueUIUpdate(screen_update)) {
            Serial.printf("[MAIN] %s screen transition queued to UI Task\n", screen_name);
        } else {
            Serial.println("[MAIN] ERROR: Failed to queue screen transition!");
        }
    }
    Serial.println("[MAIN] Boot complete - button is now fully responsive");

    // Mark this firmware as valid — cancels OTA rollback timer.
    // If this call is never reached (e.g. crash loop), the bootloader
    // reverts to the previous partition on the next reboot.
    esp_ota_mark_app_valid_cancel_rollback();
    Serial.printf("[OTA] Firmware %s marked valid — rollback cancelled\n", FW_VERSION);

    // Tasks own everything — app_main releases its 8KB stack
    Serial.println("[MAIN] FreeRTOS tasks running — app_main exiting");
    vTaskDelete(nullptr);
}
