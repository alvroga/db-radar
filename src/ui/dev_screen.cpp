#include "ui/dev_screen.h"
#include "ui/ui_manager.h"
#include "ui/navigation.h"
#include "settings_manager.h"
#include "system_logger.h"
#include "ntp_sync.h"
#include "core/arduino_compat.h"

namespace dev_screen {

// UI elements
static lv_obj_t* screen_dev = nullptr;
static lv_obj_t* logging_toggle = nullptr;
static lv_obj_t* logger_status_label = nullptr;
static lv_obj_t* ntp_status_label = nullptr;

// Event handlers
static void logging_toggle_event(lv_event_t* e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_VALUE_CHANGED) {
        bool enabled = lv_obj_has_state(logging_toggle, LV_STATE_CHECKED);

        // Update system logger
        system_logger::setEnabled(enabled);

        // Save to settings
        settings_manager::saveLoggingEnabled(enabled);

        Serial.printf("[DEV] System logging %s\n", enabled ? "enabled" : "disabled");

        // Update status display
        updateLoggerStatus();
    }
}

static void back_button_event(lv_event_t* e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        navigation::showSettings();
    }
}

static void refresh_button_event(lv_event_t* e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        updateLoggerStatus();
        updateNTPStatus();
        Serial.println("[DEV] Status refreshed");
    }
}

void create() {
    if (screen_dev) {
        Serial.println("[DEV] Screen already created");
        return;
    }

    Serial.println("[DEV] Creating DEV settings screen...");

    // Create screen
    screen_dev = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(screen_dev, lv_color_black(), 0);

    // Create header
    lv_obj_t* header = lv_obj_create(screen_dev);
    lv_obj_set_size(header, 480, 60);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(header, lv_color_hex(0x333333), 0);
    lv_obj_set_style_border_width(header, 0, 0);

    // Header title
    lv_obj_t* title = lv_label_create(header);
    lv_label_set_text(title, "Developer Settings");
    lv_obj_set_style_text_font(title, &iosevka_16, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);

    // Back button
    lv_obj_t* back_btn = lv_btn_create(header);
    lv_obj_set_size(back_btn, 80, 40);
    lv_obj_align(back_btn, LV_ALIGN_LEFT_MID, 10, 0);
    lv_obj_add_event_cb(back_btn, back_button_event, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* back_label = lv_label_create(back_btn);
    lv_label_set_text(back_label, "Back");
    lv_obj_center(back_label);

    // Refresh button
    lv_obj_t* refresh_btn = lv_btn_create(header);
    lv_obj_set_size(refresh_btn, 100, 40);
    lv_obj_align(refresh_btn, LV_ALIGN_RIGHT_MID, -10, 0);
    lv_obj_add_event_cb(refresh_btn, refresh_button_event, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* refresh_label = lv_label_create(refresh_btn);
    lv_label_set_text(refresh_label, "Refresh");
    lv_obj_center(refresh_label);

    // Content area (scrollable)
    lv_obj_t* content = lv_obj_create(screen_dev);
    lv_obj_set_size(content, 460, 400);
    lv_obj_align(content, LV_ALIGN_TOP_MID, 0, 70);
    lv_obj_set_style_bg_color(content, lv_color_black(), 0);
    lv_obj_set_style_border_width(content, 1, 0);
    lv_obj_set_style_border_color(content, lv_color_hex(0x666666), 0);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(content, 10, 0);
    lv_obj_set_style_pad_row(content, 10, 0);
    lv_obj_set_scroll_dir(content, LV_DIR_VER);

    // === LOGGING TOGGLE SECTION ===
    lv_obj_t* logging_container = lv_obj_create(content);
    lv_obj_set_size(logging_container, 440, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(logging_container, lv_color_hex(0x222222), 0);
    lv_obj_set_style_border_width(logging_container, 0, 0);
    lv_obj_set_style_pad_all(logging_container, 10, 0);

    lv_obj_t* logging_title = lv_label_create(logging_container);
    lv_label_set_text(logging_title, "System Logging");
    lv_obj_set_style_text_font(logging_title, &iosevka_16, 0);
    lv_obj_set_style_text_color(logging_title, lv_color_hex(0x00FF00), 0);
    lv_obj_align(logging_title, LV_ALIGN_TOP_LEFT, 0, 0);

    // Logging toggle switch
    logging_toggle = lv_switch_create(logging_container);
    lv_obj_align(logging_toggle, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_add_event_cb(logging_toggle, logging_toggle_event, LV_EVENT_VALUE_CHANGED, nullptr);

    // Load initial state from cached settings
    const auto& settings = settings_manager::getSettings();
    if (settings.logging_enabled) {
        lv_obj_add_state(logging_toggle, LV_STATE_CHECKED);
    }
    system_logger::setEnabled(settings.logging_enabled);

    // === LOGGER STATUS SECTION ===
    lv_obj_t* logger_container = lv_obj_create(content);
    lv_obj_set_size(logger_container, 440, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(logger_container, lv_color_hex(0x222222), 0);
    lv_obj_set_style_border_width(logger_container, 0, 0);
    lv_obj_set_style_pad_all(logger_container, 10, 0);

    lv_obj_t* logger_title = lv_label_create(logger_container);
    lv_label_set_text(logger_title, "Logger Status");
    lv_obj_set_style_text_font(logger_title, &iosevka_16, 0);
    lv_obj_set_style_text_color(logger_title, lv_color_hex(0xFFFF00), 0);
    lv_obj_align(logger_title, LV_ALIGN_TOP_LEFT, 0, 0);

    logger_status_label = lv_label_create(logger_container);
    lv_label_set_text(logger_status_label, "Loading...");
    lv_obj_set_style_text_font(logger_status_label, &iosevka_16, 0);
    lv_obj_set_style_text_color(logger_status_label, lv_color_hex(0xCCCCCC), 0);
    lv_obj_align(logger_status_label, LV_ALIGN_TOP_LEFT, 0, 30);
    lv_label_set_long_mode(logger_status_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(logger_status_label, 420);

    // === NTP STATUS SECTION ===
    lv_obj_t* ntp_container = lv_obj_create(content);
    lv_obj_set_size(ntp_container, 440, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(ntp_container, lv_color_hex(0x222222), 0);
    lv_obj_set_style_border_width(ntp_container, 0, 0);
    lv_obj_set_style_pad_all(ntp_container, 10, 0);

    lv_obj_t* ntp_title = lv_label_create(ntp_container);
    lv_label_set_text(ntp_title, "NTP Time Sync");
    lv_obj_set_style_text_font(ntp_title, &iosevka_16, 0);
    lv_obj_set_style_text_color(ntp_title, lv_color_hex(0x00FFFF), 0);
    lv_obj_align(ntp_title, LV_ALIGN_TOP_LEFT, 0, 0);

    ntp_status_label = lv_label_create(ntp_container);
    lv_label_set_text(ntp_status_label, "Loading...");
    lv_obj_set_style_text_font(ntp_status_label, &iosevka_16, 0);
    lv_obj_set_style_text_color(ntp_status_label, lv_color_hex(0xCCCCCC), 0);
    lv_obj_align(ntp_status_label, LV_ALIGN_TOP_LEFT, 0, 30);
    lv_label_set_long_mode(ntp_status_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(ntp_status_label, 420);

    // Initial status update
    updateLoggerStatus();
    updateNTPStatus();

    Serial.println("[DEV] DEV settings screen created successfully");
}

void open() {
    if (!screen_dev) {
        create();
    }

    // Update status before showing
    updateLoggerStatus();
    updateNTPStatus();

    lv_scr_load(screen_dev);
    Serial.println("[DEV] DEV screen opened");
}

void close() {
    // Nothing to do, screen remains in memory
}

void createDevTab(lv_obj_t* parent) {
    // Match padding of other tabs (GPS, WiFi, Display)
    lv_obj_set_style_pad_all(parent, 15, 0);

    int y_offset = 0;

#ifdef COMPASS_BUS_WIRE1
    lv_obj_t* build_label = lv_label_create(parent);
    lv_label_set_text(build_label, "I2C: Compass=Wire1(GPIO19/20)");
    lv_obj_set_style_text_color(build_label, lv_color_hex(0xFF8800), 0);
    lv_obj_align(build_label, LV_ALIGN_TOP_LEFT, 0, y_offset);
    y_offset += 25;
#endif

    // Load saved settings from cache to initialize logging toggle
    const auto& settings = settings_manager::getSettings();

    // === REFRESH BUTTON ===
    lv_obj_t* refresh_btn = lv_btn_create(parent);
    lv_obj_set_size(refresh_btn, 100, 35);
    lv_obj_align(refresh_btn, LV_ALIGN_TOP_RIGHT, 0, y_offset);
    lv_obj_set_style_bg_color(refresh_btn, lv_color_hex(0x00AA00), 0);  // Green
    lv_obj_set_style_border_width(refresh_btn, 0, 0);
    lv_obj_set_style_radius(refresh_btn, 8, 0);
    lv_obj_add_event_cb(refresh_btn, refresh_button_event, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* refresh_label = lv_label_create(refresh_btn);
    lv_label_set_text(refresh_label, "Refresh");
    lv_obj_set_style_text_color(refresh_label, lv_color_white(), 0);
    lv_obj_center(refresh_label);

    y_offset += 45;

    // === SYSTEM LOGGING SECTION ===
    lv_obj_t* logging_label = lv_label_create(parent);
    lv_label_set_text(logging_label, "System Logging:");
    lv_obj_set_style_text_color(logging_label, lv_color_hex(0x00FF00), 0);  // Green
    lv_obj_set_style_text_font(logging_label, &iosevka_16, 0);
    lv_obj_align(logging_label, LV_ALIGN_TOP_LEFT, 0, y_offset);

    y_offset += 30;

    // Logging toggle switch
    logging_toggle = lv_switch_create(parent);
    lv_obj_align(logging_toggle, LV_ALIGN_TOP_LEFT, 0, y_offset);
    lv_obj_add_event_cb(logging_toggle, logging_toggle_event, LV_EVENT_VALUE_CHANGED, nullptr);

    // Set initial state from settings
    if (settings.logging_enabled) {
        lv_obj_add_state(logging_toggle, LV_STATE_CHECKED);
    }
    system_logger::setEnabled(settings.logging_enabled);

    y_offset += 50;

    // === LOGGER STATUS SECTION ===
    lv_obj_t* logger_status_title = lv_label_create(parent);
    lv_label_set_text(logger_status_title, "Logger Status:");
    lv_obj_set_style_text_color(logger_status_title, lv_color_hex(0xFFFF00), 0);  // Yellow
    lv_obj_set_style_text_font(logger_status_title, &iosevka_16, 0);
    lv_obj_align(logger_status_title, LV_ALIGN_TOP_LEFT, 0, y_offset);

    y_offset += 25;

    logger_status_label = lv_label_create(parent);
    lv_label_set_text(logger_status_label, "Loading...");
    lv_obj_set_style_text_color(logger_status_label, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_style_text_font(logger_status_label, &iosevka_16, 0);
    lv_obj_align(logger_status_label, LV_ALIGN_TOP_LEFT, 0, y_offset);
    lv_label_set_long_mode(logger_status_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(logger_status_label, 330);  // Match tab width

    y_offset += 100;  // Space for multi-line status

    // === TIME SYNC STATUS SECTION (GPS/NTP) ===
    lv_obj_t* ntp_status_title = lv_label_create(parent);
    lv_label_set_text(ntp_status_title, "Time Sync (GPS/NTP):");
    lv_obj_set_style_text_color(ntp_status_title, lv_color_hex(0x00FFFF), 0);  // Cyan
    lv_obj_set_style_text_font(ntp_status_title, &iosevka_16, 0);
    lv_obj_align(ntp_status_title, LV_ALIGN_TOP_LEFT, 0, y_offset);

    y_offset += 25;

    ntp_status_label = lv_label_create(parent);
    lv_label_set_text(ntp_status_label, "Loading...");
    lv_obj_set_style_text_color(ntp_status_label, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_style_text_font(ntp_status_label, &iosevka_16, 0);
    lv_obj_align(ntp_status_label, LV_ALIGN_TOP_LEFT, 0, y_offset);
    lv_label_set_long_mode(ntp_status_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(ntp_status_label, 330);  // Match tab width

    y_offset += 130;  // Space for multi-line NTP status

    // Initial status update
    updateLoggerStatus();
    updateNTPStatus();

    // Add bottom padding spacer to allow scrolling content to middle of screen
    lv_obj_t* spacer = lv_obj_create(parent);
    lv_obj_set_size(spacer, 10, 200);  // 200px bottom padding
    lv_obj_align(spacer, LV_ALIGN_TOP_LEFT, 0, y_offset + 100);
    lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);  // Transparent
    lv_obj_set_style_border_width(spacer, 0, 0);

    Serial.println("[DEV] DEV tab populated with content");
}

void updateLoggerStatus() {
    if (!logger_status_label) return;

    char status_text[256];
    snprintf(status_text, sizeof(status_text),
             "Enabled: %s\n"
             "Log file: %s\n"
             "Buffer: %zu/%zu bytes\n"
             "File size: %zu bytes",
             system_logger::isEnabled() ? "YES" : "NO",
             system_logger::LOG_FILE,
             system_logger::getBufferUsage(),
             system_logger::BUFFER_SIZE,
             system_logger::getFileSize());

    lv_label_set_text(logger_status_label, status_text);
}

void updateNTPStatus() {
    if (!ntp_status_label) return;

    const ntp_sync::NTPSyncState& state = ntp_sync::getState();

    char time_str[64];
    ntp_sync::getTimeString(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S");

    // Calculate time since last sync
    uint32_t elapsed_sec = 0;
    if (state.last_sync_success_ms > 0) {
        elapsed_sec = (millis() - state.last_sync_success_ms) / 1000;
    }

    // Calculate time since GPS sync
    uint32_t gps_elapsed_sec = 0;
    if (state.last_gps_sync_ms > 0) {
        gps_elapsed_sec = (millis() - state.last_gps_sync_ms) / 1000;
    }

    // Calculate time since NTP sync
    uint32_t ntp_elapsed_sec = 0;
    if (state.last_ntp_sync_ms > 0) {
        ntp_elapsed_sec = (millis() - state.last_ntp_sync_ms) / 1000;
    }

    // Determine time source string
    const char* time_source_str = "Unknown";
    switch (state.time_source) {
        case ntp_sync::TimeSource::GPS:
            time_source_str = "GPS";
            break;
        case ntp_sync::TimeSource::NTP:
            time_source_str = "NTP";
            break;
        case ntp_sync::TimeSource::RTC_ONLY:
            time_source_str = "RTC Only";
            break;
    }

    char status_text[512];
    snprintf(status_text, sizeof(status_text),
             "Time Source: %s\n"
             "Synced: %s\n"
             "Current time: %s\n"
             "Last sync: %lu sec ago\n"
             "GPS syncs: %d (%lu sec ago)\n"
             "NTP syncs: %d (%lu sec ago)\n"
             "Failures: %d\n"
             "Timezone: GMT%+ld",
             time_source_str,
             state.time_synced ? "YES" : "NO",
             time_str,
             elapsed_sec,
             state.gps_sync_count,
             gps_elapsed_sec,
             state.ntp_sync_count,
             ntp_elapsed_sec,
             state.sync_failure_count,
             state.gmt_offset_sec / 3600);

    lv_label_set_text(ntp_status_label, status_text);
}

} // namespace dev_screen
