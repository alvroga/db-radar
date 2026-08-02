#include "ui/tilt_bench_screen.h"
#include "ui/navigation.h"
#include "ui/fonts/custom_fonts.h"
#include "tilt_bench.h"
#include "hardware/buzzer.h"
#include "core/arduino_compat.h"

// WP-6 bench procedure capture screen. See docs/compass_tilt_bench.md.
//
// Layout note: the panel is 480x480 but physically round -- content stays inside
// the inscribed circle, same constraint as field_log_screen.cpp. (Project memory:
// feedback_round_display_layout.)

namespace tilt_bench_screen {

static lv_obj_t* screen_tilt        = nullptr;
static lv_obj_t* label_filename     = nullptr;
static lv_obj_t* btn_pose_cycle     = nullptr;
static lv_obj_t* lbl_pose_cycle     = nullptr;
static lv_obj_t* btn_capture        = nullptr;
static lv_obj_t* label_last         = nullptr;
static lv_obj_t* btn_session        = nullptr;
static lv_obj_t* lbl_session        = nullptr;
static lv_timer_t* refresh_timer    = nullptr;

static uint8_t g_sel = 0;   // index into tilt_bench::Pose

static tilt_bench::Pose selectedPose() {
    return (tilt_bench::Pose)g_sel;
}

static void refreshUI() {
    if (!screen_tilt || !lv_obj_is_valid(screen_tilt)) return;

    bool active = tilt_bench::isSessionActive();

    if (lbl_pose_cycle && lv_obj_is_valid(lbl_pose_cycle)) {
        lv_label_set_text(lbl_pose_cycle, tilt_bench::poseName(selectedPose()));
    }

    if (label_filename && lv_obj_is_valid(label_filename)) {
        if (active) {
            lv_label_set_text_fmt(label_filename, "%s   %lu rows",
                                  tilt_bench::currentFilename(),
                                  (unsigned long)tilt_bench::rowCount());
        } else if (!tilt_bench::storageAvailable()) {
            lv_label_set_text(label_filename, "NO SD CARD");
        } else {
            lv_label_set_text_fmt(label_filename, "next: tiltbench_%03u.csv",
                                  (unsigned)(tilt_bench::lastSessionNumber() + 1));
        }
    }

    if (btn_session && lv_obj_is_valid(btn_session)) {
        lv_obj_set_style_bg_color(btn_session,
            active ? lv_color_hex(0xCC0000) : lv_color_hex(0x00AA00), 0);
        lv_label_set_text(lbl_session, active ? "End Session" : "Start Session");
    }

    if (btn_capture && lv_obj_is_valid(btn_capture)) {
        if (active) lv_obj_clear_state(btn_capture, LV_STATE_DISABLED);
        else        lv_obj_add_state(btn_capture, LV_STATE_DISABLED);
    }
}

static void pose_cycle_event(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    g_sel = (uint8_t)((g_sel + 1) % (uint8_t)tilt_bench::Pose::COUNT);
    buzzer::chirp(15);
    refreshUI();
}

static void capture_event(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!tilt_bench::isSessionActive()) return;

    tilt_bench::CaptureResult r = tilt_bench::capture(selectedPose());

    if (!r.ok) {
        buzzer::rapidPulseAsync();
        if (label_last && lv_obj_is_valid(label_last)) {
            lv_label_set_text(label_last, "CAPTURE FAILED\ncheck accel/mag");
            lv_obj_set_style_text_color(label_last, lv_color_hex(0xFF4444), 0);
        }
        refreshUI();
        return;
    }

    buzzer::chirp(15);
    if (label_last && lv_obj_is_valid(label_last)) {
        lv_obj_set_style_text_color(label_last, lv_color_hex(0xCCCCCC), 0);
        lv_label_set_text_fmt(label_last,
            "%s\nax %+.3f ay %+.3f az %+.3f\ncx %+d cy %+d cz %+d\nhdg %.1f",
            tilt_bench::poseName(selectedPose()),
            r.ax_g, r.ay_g, r.az_g, r.cx, r.cy, r.cz, r.heading_deg);
    }

    // Auto-advance so the user doesn't also have to tap the pose selector between
    // captures -- one tap per pose while holding an awkward orientation.
    g_sel = (uint8_t)((g_sel + 1) % (uint8_t)tilt_bench::Pose::COUNT);
    refreshUI();
}

static void session_event(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    if (tilt_bench::isSessionActive()) {
        tilt_bench::endSession();
        buzzer::doubleBeep();
    } else {
        if (tilt_bench::startSession()) {
            g_sel = 0;   // start the protocol at FLAT
            buzzer::chirp(60);
        } else {
            buzzer::rapidPulseAsync();
        }
    }
    refreshUI();
}

static void refresh_timer_cb(lv_timer_t*) {
    // Self-suspend rather than trusting close() to be called -- same pattern as
    // field_log_screen, for the same reason (any other route off this screen would
    // otherwise leave the timer running against a hidden screen).
    if (!screen_tilt || lv_scr_act() != screen_tilt) {
        close();
        return;
    }
    refreshUI();
}

static void back_event(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    close();
    navigation::showSettings();
}

void create() {
    if (screen_tilt) return;

    screen_tilt = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(screen_tilt, lv_color_black(), 0);
    lv_obj_clear_flag(screen_tilt, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title = lv_label_create(screen_tilt);
    lv_label_set_text(title, "TILT BENCH");
    lv_obj_set_style_text_font(title, &iosevka_20, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x00FFFF), 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -190);

    label_filename = lv_label_create(screen_tilt);
    lv_label_set_text(label_filename, "next: -");
    lv_obj_set_style_text_font(label_filename, &iosevka_14, 0);
    lv_obj_set_style_text_color(label_filename, lv_color_hex(0x888888), 0);
    lv_obj_align(label_filename, LV_ALIGN_CENTER, 0, -160);

    // --- pose selector: cycles the 6-pose protocol, also auto-advanced by Capture ---
    btn_pose_cycle = lv_btn_create(screen_tilt);
    lv_obj_set_size(btn_pose_cycle, 280, 50);
    lv_obj_align(btn_pose_cycle, LV_ALIGN_CENTER, 0, -110);
    lv_obj_set_style_bg_color(btn_pose_cycle, lv_color_hex(0x333366), 0);
    lv_obj_set_style_radius(btn_pose_cycle, 10, 0);
    lv_obj_add_event_cb(btn_pose_cycle, pose_cycle_event, LV_EVENT_CLICKED, nullptr);

    lbl_pose_cycle = lv_label_create(btn_pose_cycle);
    lv_label_set_text(lbl_pose_cycle, "flat");
    lv_obj_set_style_text_font(lbl_pose_cycle, &iosevka_20, 0);
    lv_obj_center(lbl_pose_cycle);

    // --- CAPTURE: large and centred, must be hittable one-handed while holding the
    //     device vertical or on its side ---
    btn_capture = lv_btn_create(screen_tilt);
    lv_obj_set_size(btn_capture, 220, 90);
    lv_obj_align(btn_capture, LV_ALIGN_CENTER, 0, -20);
    lv_obj_set_style_bg_color(btn_capture, lv_color_hex(0x0088CC), 0);
    lv_obj_set_style_radius(btn_capture, 14, 0);
    lv_obj_add_event_cb(btn_capture, capture_event, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_state(btn_capture, LV_STATE_DISABLED);

    lv_obj_t* lbl_capture = lv_label_create(btn_capture);
    lv_label_set_text(lbl_capture, "CAPTURE");
    lv_obj_set_style_text_font(lbl_capture, &iosevka_20, 0);
    lv_obj_center(lbl_capture);

    label_last = lv_label_create(screen_tilt);
    lv_label_set_text(label_last, "no capture yet");
    lv_obj_set_style_text_font(label_last, &iosevka_14, 0);
    lv_obj_set_style_text_color(label_last, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_style_text_align(label_last, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(label_last, 320);
    lv_obj_align(label_last, LV_ALIGN_CENTER, 0, 70);

    btn_session = lv_btn_create(screen_tilt);
    lv_obj_set_size(btn_session, 220, 50);
    lv_obj_align(btn_session, LV_ALIGN_CENTER, 0, 155);
    lv_obj_set_style_bg_color(btn_session, lv_color_hex(0x00AA00), 0);
    lv_obj_set_style_radius(btn_session, 10, 0);
    lv_obj_add_event_cb(btn_session, session_event, LV_EVENT_CLICKED, nullptr);

    lbl_session = lv_label_create(btn_session);
    lv_label_set_text(lbl_session, "Start Session");
    lv_obj_set_style_text_font(lbl_session, &iosevka_16, 0);
    lv_obj_center(lbl_session);

    lv_obj_t* btn_back = lv_btn_create(screen_tilt);
    lv_obj_set_size(btn_back, 120, 42);
    lv_obj_align(btn_back, LV_ALIGN_CENTER, 0, 205);
    lv_obj_set_style_bg_color(btn_back, lv_color_hex(0x444444), 0);
    lv_obj_set_style_radius(btn_back, 10, 0);
    lv_obj_add_event_cb(btn_back, back_event, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* lbl_back = lv_label_create(btn_back);
    lv_label_set_text(lbl_back, "Back");
    lv_obj_center(lbl_back);

    Serial.println("[TILTBENCH-UI] Tilt Bench screen created");
}

void open() {
    if (!screen_tilt) create();

    if (!refresh_timer) {
        refresh_timer = lv_timer_create(refresh_timer_cb, 500, nullptr);
    }

    refreshUI();
    lv_scr_load(screen_tilt);
}

void close() {
    if (refresh_timer) {
        lv_timer_del(refresh_timer);
        refresh_timer = nullptr;
    }
}

} // namespace tilt_bench_screen
