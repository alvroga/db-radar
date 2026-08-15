// Waveshare ESP32-S3 Touch LCD 2.1"
// GPS Radar — ESP-IDF entry point

#include "core/arduino_compat.h"
#include <lvgl.h>
#include "esp_ota_ops.h"
#include "esp_system.h"

// Modular components
#include "core/device_manager.h"
#include "ui/ui_manager.h"
#include "ui/navigation.h"
#include "ui/settings_screen.h"
#include "utils/diagnostics.h"
#include "utils/task_manager.h"
#include "utils/system_logger.h"
#include "utils/field_log.h"
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

// Pumps LVGL alongside a loading-screen status update, so the boot spinner keeps
// animating between init phases instead of sitting on one frame for seconds at a
// time. Safe ONLY here, before task_manager::startTasks() — every call site in this
// file's boot sequence runs while LVGL is still single-threaded (see the "UI Task
// owns LVGL after startTasks()" comment further down). Do not add a call site after
// startTasks(), and do not move this pattern into ui_manager::updateLoadingStatus()
// itself (see its own NOTE) since that function has callers whose threading context
// it can't verify.
static void setLoadingStatus(const char* message) {
    ui_manager::updateLoadingStatus(message);
    lv_task_handler();
}

// One-time first-boot GPS module picker. Shown only when settings.gps_module_configured
// is false; once the user picks, device_manager::initGPS() skips protocol auto-detection
// on every future boot (see gps_bh880::beginWithProtocol()). GPS itself already ran full
// auto-detect THIS boot (device_manager::initializeAll(), before display was even up), so
// picking doesn't affect GPS this session — but it reboots anyway (see the esp_restart()
// call at the end) because device_manager::initCompass() also already ran during Phase 2,
// BEFORE this picker could possibly have shown, and with no pin yet it skipped compass
// entirely rather than auto-probing (compass_qmc5883l.h). Nothing re-runs initCompass()
// later in the same boot, so without the reboot a freshly-picked BH-880/BN-880 board would
// finish its first session with no working compass despite Settings showing the correct
// pin — field-caught 2026-08-11.
// Blocking here on a tap is safe: LVGL is still single-threaded (task_manager::startTasks()
// hasn't run yet) — same precondition setLoadingStatus() above relies on.
// Returns the picker object so the caller can delete it, kept as defense-in-depth even
// though the reboot below means this normally never returns: the caller must load a
// different screen first (see the LVGL screen-lifecycle rule: never delete the active
// screen before its replacement loads). Deleting it here, before screen_loading exists,
// crashed lv_scr_load()'s next call (LoadProhibited inside lv_scr_load_anim/lv_obj_set_pos,
// dereferencing the freed picker as the still-recorded previous active screen) —
// field-caught 2026-08-11 (the same session that found the compass gap above).
static lv_obj_t* showGpsModulePickerBlocking() {
    Serial.println("[MAIN] First boot — showing GPS module picker");

    lv_obj_t* picker = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(picker, lv_color_hex(0x1A1A1A), 0);
    lv_obj_set_style_bg_opa(picker, LV_OPA_COVER, 0);
    lv_obj_clear_flag(picker, LV_OBJ_FLAG_SCROLLABLE);
    lv_scr_load(picker);

    lv_obj_t* title = lv_label_create(picker);
    lv_label_set_text(title, "Select GPS Module");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, &iosevka_16, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 80);

    char detected[56];
    snprintf(detected, sizeof(detected), "Detected this boot: %s", gps_bh880::protocolName());
    lv_obj_t* subtitle = lv_label_create(picker);
    lv_label_set_text(subtitle, detected);
    lv_obj_set_style_text_color(subtitle, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(subtitle, &iosevka_16, 0);
    lv_obj_align(subtitle, LV_ALIGN_TOP_MID, 0, 108);

    static bool s_picked = false;
    static uint8_t s_picked_type = 0;
    s_picked = false;

    // Three stacked buttons — heights/gaps compressed from the original two-button
    // layout (55px/70px) to 50px/58px so all three plus the hint label still clear
    // the round display's safe area (verified: at y=325, 300px from top edge, the
    // circular chord width is still ~330px, comfortably more than the 300px hint).
    lv_obj_t* btn_bh880 = lv_btn_create(picker);
    lv_obj_set_size(btn_bh880, 220, 50);
    lv_obj_align(btn_bh880, LV_ALIGN_TOP_MID, 0, 145);
    lv_obj_set_style_bg_color(btn_bh880, lv_color_hex(0x0080FF), 0);
    lv_obj_add_event_cb(btn_bh880, [](lv_event_t*) { s_picked_type = 0; s_picked = true; }, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* lbl_bh880 = lv_label_create(btn_bh880);
    lv_label_set_text(lbl_bh880, "BH-880\n(GPS + Compass)");
    lv_label_set_long_mode(lbl_bh880, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(lbl_bh880, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(lbl_bh880, &iosevka_16, 0);
    lv_obj_center(lbl_bh880);

    lv_obj_t* btn_bn880 = lv_btn_create(picker);
    lv_obj_set_size(btn_bn880, 220, 50);
    lv_obj_align(btn_bn880, LV_ALIGN_TOP_MID, 0, 203);
    lv_obj_set_style_bg_color(btn_bn880, lv_color_hex(0x0080FF), 0);
    lv_obj_add_event_cb(btn_bn880, [](lv_event_t*) { s_picked_type = 2; s_picked = true; }, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* lbl_bn880 = lv_label_create(btn_bn880);
    lv_label_set_text(lbl_bn880, "BN-880\n(GPS + Compass)");
    lv_label_set_long_mode(lbl_bn880, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(lbl_bn880, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(lbl_bn880, &iosevka_16, 0);
    lv_obj_center(lbl_bn880);

    lv_obj_t* btn_lc76g = lv_btn_create(picker);
    lv_obj_set_size(btn_lc76g, 220, 50);
    lv_obj_align(btn_lc76g, LV_ALIGN_TOP_MID, 0, 261);
    lv_obj_set_style_bg_color(btn_lc76g, lv_color_hex(0x0080FF), 0);
    lv_obj_add_event_cb(btn_lc76g, [](lv_event_t*) { s_picked_type = 1; s_picked = true; }, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* lbl_lc76g = lv_label_create(btn_lc76g);
    lv_label_set_text(lbl_lc76g, "LC76G\n(GPS only, North-Up)");
    lv_label_set_long_mode(lbl_lc76g, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(lbl_lc76g, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(lbl_lc76g, &iosevka_16, 0);
    lv_obj_center(lbl_lc76g);

    lv_obj_t* hint = lv_label_create(picker);
    lv_label_set_text(hint, "One-time choice - change later in\nSettings > GPS");
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x666666), 0);
    lv_obj_set_style_text_font(hint, &iosevka_16, 0);
    lv_obj_set_width(hint, 300);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 325);

    lv_task_handler();
    while (!s_picked) {
        lv_task_handler();
        delay(5);
    }

    settings_manager::saveGPSModuleSelection(s_picked_type);
    Serial.printf("[MAIN] GPS module selected: %s\n", gps_bh880::moduleTypeName(s_picked_type));

    // Reboot rather than continuing this boot — unlike GPS (which already ran full
    // auto-detect before the picker even showed, so it works this session regardless of
    // the pick), device_manager::initCompass() ran during Phase 2, BEFORE this picker was
    // ever shown, and with gps_module_configured still false at that point it skipped
    // compass entirely (see compass_qmc5883l.h's no-auto-probe design). Nothing re-runs
    // initCompass() later in this same boot, so without a reboot here, a BH-880/BN-880
    // board would finish this first session with a correctly-pinned Settings page but a
    // genuinely uninitialized compass — field-caught 2026-08-11 (looked exactly like the
    // LC76G/no-compass profile despite BH-880 being selected). Matches the reboot every
    // other module-pin-change path already uses (Settings > GPS button, `gps module set`).
    Serial.println("[MAIN] Rebooting to apply module selection...");
    delay(200);
    esp_restart();

    return picker;  // unreachable — esp_restart() does not return
}

extern "C" void app_main() {
    // Serial is USB CDC — begin() is a no-op in ESP-IDF
    Serial.begin(115200);
    uint32_t t0 = millis();
    while (!Serial && (millis() - t0 < 1500)) { delay(10); }

    // BOOT PHASE 1: Serial initialized
    Serial.printf("[T+%08lu] BOOT: Phase 1 - Serial initialized\n", millis());

    // Printed unconditionally (not gated behind the `version` serial command) so
    // every field-captured boot log self-identifies which firmware produced it —
    // otherwise the only way to tell is cross-referencing the ESP-IDF bootloader's
    // own "compile time" line against git commit timestamps by hand.
    Serial.printf("[BOOT] Running DRAC OS %s\n", FW_VERSION);

    Serial.println("==== GPS Radar - FreeRTOS Task Architecture ====");
    Serial.printf("[BOOT] CPU: %lu MHz  (expected 240 — a 160 here means "
                  "CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240 did not reach sdkconfig)\n",
                  (unsigned long)getCpuFrequencyMhz());

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
                // Field data logger — DEV mode only. Allocates a 512-row PSRAM ring
                // and a writer task; both are pure overhead outside a field trip,
                // so they never exist in normal mode.
                field_log::begin();

                Serial.println("[DEV] ==========================================");
                Serial.println("[DEV] DEV MODE ACTIVE at boot");
                Serial.println("[DEV] ==========================================");

                // This banner used to claim UI Task checkpoints, button events, and
                // queue operations were logged to SD — none of those were ever wired
                // up, and it said "30 seconds" while the code said 120. Found and
                // fixed 2026-08-02 while building I2C forensic logging for FT-06
                // (docs/i2c_bus_freeze_investigation.md): a banner nobody could see
                // failing next to the one real log line (a 2-minute heartbeat) is
                // exactly why the SD log looked "almost empty" in the field. Describe
                // only what actually happens now.
                system_logger::info("DEV", "==========================================");
                system_logger::info("DEV", "DEV MODE — SD LOGGING ACTIVE");
                system_logger::info("DEV", "- I2C failures/slow ops: logged as they happen");
                system_logger::info("DEV", "- I2C per-device stats + heartbeat: every 60s");
                system_logger::info("DEV", "- EXIO register canary: every 10s");
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

    // First boot only: pin the GPS module (BH-880 vs LC76G) so every future boot skips
    // protocol auto-detection entirely. See device_manager::initGPS() and
    // gps_bh880::beginWithProtocol(). GPS already ran full auto-detect this session
    // (device_manager::initializeAll(), before display was up) — this picker doesn't
    // block THIS boot's GPS, it only determines what subsequent boots do.
    lv_obj_t* gps_picker = nullptr;
    if (!settings_manager::getSettings().gps_module_configured) {
        gps_picker = showGpsModulePickerBlocking();
    }

    // Create and display loading screen
    Serial.println("[MAIN] Displaying loading screen...");
    ui_manager::createLoadingScreen();
    ui_manager::UIState& ui = ui_manager::getUIState();
    lv_scr_load(ui.screen_loading);
    // Only safe to delete the picker now that it's no longer the active screen — see the
    // comment on showGpsModulePickerBlocking().
    if (gps_picker) {
        lv_obj_del(gps_picker);
    }
    device_manager::enableLVGLProcessing();
    lv_task_handler();
    Serial.println("[MAIN] Loading screen displayed");

    setLoadingStatus("Warming up circuits...");
    delay(200);

    // Dedicated boot modes: create the appropriate full-screen UI
    {
        const auto& boot = settings_manager::getSettings();
        if (boot.wifi_ap_enabled) {
            setLoadingStatus("Starting upload mode...");
            ui_manager::createAPScreen();
            Serial.println("[MAIN] AP upload screen created");
        } else if (boot.wifi_sta_boot) {
            setLoadingStatus("Starting WiFi mode...");
            ui_manager::createWiFiScreen();
            Serial.println("[MAIN] WiFi STA screen created");
        }
    }

    // Pre-create settings screen (eliminates first-press delay)
    setLoadingStatus("Building control panels...");
    Serial.println("[MAIN] Pre-creating settings screen...");
    uint32_t settings_start = millis();
    ui_manager::createSettingsScreen();
    Serial.printf("[MAIN] ✓ Settings screen ready (%.1fs)\n",
                  (millis() - settings_start) / 1000.0);

    // Initialize navigation system
    setLoadingStatus("Calibrating compass...");
    if (!navigation::init()) {
        Serial.println("[ERROR] Navigation initialization failed");
        while (1) { delay(1000); }
    }

    // Initialize diagnostics
    setLoadingStatus("Running diagnostics...");
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
    setLoadingStatus("Scanning for networks...");
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

    // Auto-load GPX files from FFat flash storage (moved off SD, see ADR-0024)
    setLoadingStatus("Loading waypoints...");
    Serial.println("[GPX] Auto-loading waypoints from /ffat/gpx/ folder...");
    // gpx_loader::init() allocates the PSRAM two-tier index (gpx_index + selection
    // scratch) — was never called anywhere before this, so the two-tier path was
    // dead code and every load silently used the file-order legacy fallback.
    gpx_loader::init();
    int waypoints_loaded = gpx_loader::loadAllGPXFiles(true);  // show_boot_progress
    if (waypoints_loaded > 0) {
        navigation::updateRadarDisplay();
    } else {
        Serial.println("[GPX] No waypoints loaded (folder empty or SD unavailable)");
    }

    // Initialize Task Watchdog Timer
    setLoadingStatus("Arming watchdog...");
    Serial.println("[WATCHDOG] Initializing Task Watchdog Timer...");
    watchdog::Config wdt_config;
    wdt_config.timeout_seconds = 30;
    // Panics (resets) on timeout instead of only logging. A System Task I2C call
    // that blocks past its own driver timeout (see FT-06, 2026-08-02: two fast
    // COMPASS ESP_ERR_INVALID_STATE hits at 04:24, then total silence with
    // i2c_consec=0 and no next boot in system(3).log) is invisible to
    // checkI2CBusHealth() — that detector only counts calls that return, so a
    // hung call never reaches its 10-consecutive-failure threshold and the
    // device just sits wedged forever. A TWDT reset turns that into a clean
    // reboot instead, and the next boot's "Reset reason: TASK_WDT" confirms
    // this was the mechanism if it recurs.
    wdt_config.panic_on_timeout = true;
    wdt_config.enable_idle_task = false;

    if (!watchdog::init(wdt_config)) {
        Serial.println("[WARN] Watchdog initialization failed");
    } else {
        Serial.println("[WATCHDOG] TWDT initialized with 30s timeout (panic=true)");
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
    setLoadingStatus(boot_messages[idx]);
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
