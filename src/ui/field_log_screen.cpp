#include "ui/field_log_screen.h"
#include "ui/navigation.h"
#include "ui/fonts/custom_fonts.h"
#include "field_log.h"
#include "settings_manager.h"
#include "accel_qmi8658.h"
#include "hardware/buzzer.h"
#include "core/arduino_compat.h"

// Field sample capture. See docs/compass_calibration_foundation.md §8.2.
//
// Layout note: the panel is 480x480 but **physically round**. Content must stay
// inside the inscribed circle — at |y| = 200 from centre only ~264px of width is
// visible, so anything wide belongs near the middle. Absolute top/bottom
// positioning clips. (Project memory: feedback_round_display_layout.)

namespace field_log_screen {

static lv_obj_t* screen_flog       = nullptr;
static lv_obj_t* label_next_file   = nullptr;
static lv_obj_t* btn_label_cycle   = nullptr;
static lv_obj_t* lbl_label_cycle   = nullptr;
static lv_obj_t* btn_start_stop    = nullptr;
static lv_obj_t* lbl_start_stop    = nullptr;
static lv_obj_t* label_status      = nullptr;
static lv_obj_t* sw_accel          = nullptr;
static lv_obj_t* label_accel       = nullptr;
static lv_timer_t* refresh_timer   = nullptr;

static uint8_t g_sel = 0;   // index into field_log::Label

static field_log::Label selectedLabel() {
    return (field_log::Label)g_sel;
}

static void refreshUI() {
    if (!screen_flog || !lv_obj_is_valid(screen_flog)) return;

    field_log::Stats st = field_log::stats();

    if (lbl_label_cycle && lv_obj_is_valid(lbl_label_cycle)) {
        lv_label_set_text(lbl_label_cycle, field_log::labelName(selectedLabel()));
    }

    if (label_next_file && lv_obj_is_valid(label_next_file)) {
        if (st.recording) {
            lv_label_set_text(label_next_file, st.filename);
        } else {
            lv_label_set_text_fmt(label_next_file, "next: cal_%03u_%s.csv",
                                  (unsigned)(field_log::lastSampleNumber() + 1),
                                  field_log::labelName(selectedLabel()));
        }
    }

    if (btn_start_stop && lv_obj_is_valid(btn_start_stop)) {
        lv_obj_set_style_bg_color(btn_start_stop,
            st.recording ? lv_color_hex(0xCC0000) : lv_color_hex(0x00AA00), 0);
        lv_label_set_text(lbl_start_stop, st.recording ? "STOP" : "START");
    }

    // Cycling the label mid-sample would misname the file that is already open.
    if (btn_label_cycle && lv_obj_is_valid(btn_label_cycle)) {
        if (st.recording) lv_obj_add_state(btn_label_cycle, LV_STATE_DISABLED);
        else              lv_obj_clear_state(btn_label_cycle, LV_STATE_DISABLED);
    }

    if (label_status && lv_obj_is_valid(label_status)) {
        if (!field_log::storageAvailable()) {
            lv_label_set_text(label_status, "NO SD CARD\nnowhere to write");
            lv_obj_set_style_text_color(label_status, lv_color_hex(0xFF4444), 0);
        } else if (st.recording) {
            lv_obj_set_style_text_color(label_status, lv_color_hex(0x00FF88), 0);
            lv_label_set_text_fmt(label_status,
                "%lu.%lus   %lu rows\n%lu KB   %llu MB free%s",
                (unsigned long)(st.elapsed_ms / 1000),
                (unsigned long)((st.elapsed_ms % 1000) / 100),
                (unsigned long)st.rows_written,
                (unsigned long)(st.file_bytes / 1024),
                (unsigned long long)(st.free_bytes / (1024 * 1024)),
                st.rows_dropped ? "\nDROPPED ROWS!" : "");
        } else {
            lv_obj_set_style_text_color(label_status, lv_color_hex(0xCCCCCC), 0);
            lv_label_set_text_fmt(label_status, "idle\n%llu MB free",
                                  (unsigned long long)(st.free_bytes / (1024 * 1024)));
        }
    }

    if (label_accel && lv_obj_is_valid(label_accel)) {
        lv_label_set_text_fmt(label_accel, "accel %s",
                              accel_qmi8658::isInitialized() ? "ok" : "OFF");
    }
}

static void refresh_timer_cb(lv_timer_t*) {
    // Self-suspend rather than trusting close() to be called. Any other route off
    // this screen (a queued LOAD_RADAR_SCREEN, standby wake, the settings screen
    // reloading itself) would otherwise leave the timer running forever against a
    // screen nobody is looking at.
    if (!screen_flog || lv_scr_act() != screen_flog) {
        close();
        return;
    }
    refreshUI();
}

static void label_cycle_event(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (field_log::isRecording()) return;

    g_sel = (uint8_t)((g_sel + 1) % (uint8_t)field_log::Label::COUNT);

    // The gyro is only wanted for the 100Hz shake sample: it cannot supply heading
    // and costs power, so it follows the label selection rather than being a
    // separate thing to remember. See doc §6A.
    bool want_gyro = (selectedLabel() == field_log::Label::SHAKE_100HZ);
    if (want_gyro != accel_qmi8658::isGyroEnabled()) {
        accel_qmi8658::setGyroEnabled(want_gyro);
    }

    buzzer::chirp(15);
    refreshUI();
}

static void start_stop_event(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    if (field_log::isRecording()) {
        field_log::stopSample();
        // Double-beep on stop, single chirp on start — deliberately distinguishable.
        // ⚠️ Non-negotiable per doc §8.2: half these samples are taken while
        // tumbling the device or holding it phone-style, i.e. NOT looking at the
        // screen. Silent start/stop produces empty or doubled files, and you only
        // find out after the trip.
        buzzer::doubleBeep();
    } else {
        if (field_log::startSample(selectedLabel())) {
            buzzer::chirp(60);
        } else {
            // Failed to start — a distinct rapid pattern, so it is not mistaken
            // for a successful start out in the field.
            buzzer::rapidPulseAsync();
        }
    }
    refreshUI();
}

static void accel_switch_event(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    bool on = lv_obj_has_state(sw_accel, LV_STATE_CHECKED);
    accel_qmi8658::setEnabled(on);
    settings_manager::saveAccelEnabled(on);
    if (on && !accel_qmi8658::isInitialized()) {
        accel_qmi8658::begin(accel_qmi8658::isGyroEnabled());
    }
    refreshUI();
}

static void back_event(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    close();
    navigation::showSettings();
}

void create() {
    if (screen_flog) return;

    screen_flog = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(screen_flog, lv_color_black(), 0);
    lv_obj_clear_flag(screen_flog, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title = lv_label_create(screen_flog);
    lv_label_set_text(title, "FIELD LOG");
    lv_obj_set_style_text_font(title, &iosevka_20, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x00FFFF), 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -180);

    label_next_file = lv_label_create(screen_flog);
    lv_label_set_text(label_next_file, "next: -");
    lv_obj_set_style_text_font(label_next_file, &iosevka_14, 0);
    lv_obj_set_style_text_color(label_next_file, lv_color_hex(0x888888), 0);
    lv_obj_align(label_next_file, LV_ALIGN_CENTER, 0, -148);

    // --- label selector: one button, cycles the fixed §8.3 vocabulary ---
    btn_label_cycle = lv_btn_create(screen_flog);
    lv_obj_set_size(btn_label_cycle, 300, 56);
    lv_obj_align(btn_label_cycle, LV_ALIGN_CENTER, 0, -95);
    lv_obj_set_style_bg_color(btn_label_cycle, lv_color_hex(0x333366), 0);
    lv_obj_set_style_radius(btn_label_cycle, 10, 0);
    lv_obj_add_event_cb(btn_label_cycle, label_cycle_event, LV_EVENT_CLICKED, nullptr);

    lbl_label_cycle = lv_label_create(btn_label_cycle);
    lv_label_set_text(lbl_label_cycle, "flat360");
    lv_obj_set_style_text_font(lbl_label_cycle, &iosevka_20, 0);
    lv_obj_center(lbl_label_cycle);

    // --- START / STOP: large and centred, must be hittable one-handed while
    //     holding the device in an awkward posture ---
    btn_start_stop = lv_btn_create(screen_flog);
    lv_obj_set_size(btn_start_stop, 260, 104);
    lv_obj_align(btn_start_stop, LV_ALIGN_CENTER, 0, 5);
    lv_obj_set_style_bg_color(btn_start_stop, lv_color_hex(0x00AA00), 0);
    lv_obj_set_style_radius(btn_start_stop, 14, 0);
    lv_obj_add_event_cb(btn_start_stop, start_stop_event, LV_EVENT_CLICKED, nullptr);

    lbl_start_stop = lv_label_create(btn_start_stop);
    lv_label_set_text(lbl_start_stop, "START");
    lv_obj_set_style_text_font(lbl_start_stop, &iosevka_20, 0);
    lv_obj_center(lbl_start_stop);

    label_status = lv_label_create(screen_flog);
    lv_label_set_text(label_status, "idle");
    lv_obj_set_style_text_font(label_status, &iosevka_16, 0);
    lv_obj_set_style_text_color(label_status, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_style_text_align(label_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(label_status, 320);
    lv_obj_align(label_status, LV_ALIGN_CENTER, 0, 100);

    // --- accelerometer kill switch (doc §7.4) ---
    label_accel = lv_label_create(screen_flog);
    lv_label_set_text(label_accel, "accel");
    lv_obj_set_style_text_font(label_accel, &iosevka_14, 0);
    lv_obj_set_style_text_color(label_accel, lv_color_hex(0xAAAAAA), 0);
    lv_obj_align(label_accel, LV_ALIGN_CENTER, -60, 160);

    sw_accel = lv_switch_create(screen_flog);
    lv_obj_align(sw_accel, LV_ALIGN_CENTER, 40, 160);
    lv_obj_add_event_cb(sw_accel, accel_switch_event, LV_EVENT_VALUE_CHANGED, nullptr);

    lv_obj_t* btn_back = lv_btn_create(screen_flog);
    lv_obj_set_size(btn_back, 120, 42);
    lv_obj_align(btn_back, LV_ALIGN_CENTER, 0, 205);
    lv_obj_set_style_bg_color(btn_back, lv_color_hex(0x444444), 0);
    lv_obj_set_style_radius(btn_back, 10, 0);
    lv_obj_add_event_cb(btn_back, back_event, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* lbl_back = lv_label_create(btn_back);
    lv_label_set_text(lbl_back, "Back");
    lv_obj_center(lbl_back);

    Serial.println("[FLOG-UI] Field Log screen created");
}

void open() {
    if (!screen_flog) create();

    if (sw_accel && lv_obj_is_valid(sw_accel)) {
        if (settings_manager::getSettings().accel_enabled) lv_obj_add_state(sw_accel, LV_STATE_CHECKED);
        else                                               lv_obj_clear_state(sw_accel, LV_STATE_CHECKED);
    }

    // Live readout while recording. 500ms is fast enough to confirm rows are
    // accumulating and slow enough to be invisible in the frame budget.
    if (!refresh_timer) {
        refresh_timer = lv_timer_create(refresh_timer_cb, 500, nullptr);
    }

    refreshUI();
    lv_scr_load(screen_flog);
}

void close() {
    // Delete the timer rather than leaving it running against a hidden screen —
    // it would keep calling stats(), which hits the filesystem for free space.
    if (refresh_timer) {
        lv_timer_del(refresh_timer);
        refresh_timer = nullptr;
    }
}

} // namespace field_log_screen
