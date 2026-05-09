#include "ui/waypoint_screen.h"
#include "ui/ui_manager.h"
#include "ui/navigation.h"
#include <lvgl.h>
#include "core/arduino_compat.h"

namespace waypoint_screen {

void close() {
    ui_manager::UIState& ui = ui_manager::getUIState();
    if (ui.screen_waypoint && lv_obj_is_valid(ui.screen_waypoint)) {
        lv_obj_del(ui.screen_waypoint);
    }
    ui.screen_waypoint = nullptr;
    ui.selected_waypoint_index = -1;
}

void open() {
    ui_manager::UIState& ui = ui_manager::getUIState();

    int idx = ui.selected_waypoint_index;
    if (idx < 0 || idx >= ui.waypoint_count || !ui.waypoints[idx].valid) {
        Serial.println("[WPT_SCREEN] Invalid waypoint index — aborting");
        return;
    }

    // Destroy previous instance if it exists
    close();
    ui.selected_waypoint_index = idx;  // close() cleared it, restore

    const ui_manager::Waypoint& wp = ui.waypoints[idx];

    Serial.printf("[WPT_SCREEN] Opening detail for: %s\n",
                  wp.display_name[0] ? wp.display_name : wp.name);

    // =========================================================================
    // SCREEN — dark background matching settings screen
    // =========================================================================
    ui.screen_waypoint = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(ui.screen_waypoint, lv_color_hex(0x1A1A1A), 0);
    lv_obj_set_style_bg_opa(ui.screen_waypoint, LV_OPA_COVER, 0);

    // =========================================================================
    // HEADER BAR (inset for circular display, matching settings screen geometry)
    // =========================================================================
    lv_obj_t* header = lv_obj_create(ui.screen_waypoint);
    lv_obj_set_size(header, 360, 50);
    lv_obj_set_pos(header, 60, 0);
    lv_obj_set_style_bg_color(header, lv_color_hex(0x2A2A2A), 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_pad_all(header, 0, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    // Cache code in header (e.g. "GC38EVJ")
    lv_obj_t* lbl_code = lv_label_create(header);
    lv_label_set_text(lbl_code, wp.name[0] ? wp.name : "Waypoint");
    lv_obj_set_style_text_color(lbl_code, lv_color_hex(0x00FF00), 0);  // Green like radar theme
    lv_obj_set_style_text_font(lbl_code, &iosevka_16, 0);
    lv_obj_align(lbl_code, LV_ALIGN_CENTER, 0, 0);

    // =========================================================================
    // SCROLLABLE CONTENT PANEL (inset same as settings tabview)
    // =========================================================================
    lv_obj_t* panel = lv_obj_create(ui.screen_waypoint);
    lv_obj_set_pos(panel, 60, 50);
    lv_obj_set_size(panel, 360, 430);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x1A1A1A), 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_pad_all(panel, 15, 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(panel, 12, 0);

    // =========================================================================
    // FOUND BANNER — shown at top when waypoint has been marked found
    // =========================================================================
    if (wp.found) {
        lv_obj_t* found_row = lv_obj_create(panel);
        lv_obj_set_width(found_row, 330);
        lv_obj_set_height(found_row, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(found_row, lv_color_hex(0x003A00), 0);
        lv_obj_set_style_bg_opa(found_row, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(found_row, 8, 0);
        lv_obj_set_style_border_width(found_row, 0, 0);
        lv_obj_set_style_pad_all(found_row, 8, 0);
        lv_obj_clear_flag(found_row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* found_lbl = lv_label_create(found_row);
        lv_label_set_text(found_lbl, "FOUND");
        lv_obj_set_style_text_color(found_lbl, lv_color_hex(0x00FF44), 0);
        lv_obj_set_style_text_font(found_lbl, &iosevka_16, 0);
        lv_obj_center(found_lbl);
    }

    // =========================================================================
    // TITLE SECTION
    // =========================================================================
    const char* title = wp.display_name[0] ? wp.display_name : (wp.name[0] ? wp.name : "Unnamed Waypoint");

    lv_obj_t* lbl_title_hdr = lv_label_create(panel);
    lv_label_set_text(lbl_title_hdr, "NAME");
    lv_obj_set_style_text_color(lbl_title_hdr, lv_color_hex(0x00AA00), 0);
    lv_obj_set_style_text_font(lbl_title_hdr, &iosevka_16, 0);
    lv_obj_set_width(lbl_title_hdr, 330);

    lv_obj_t* lbl_title = lv_label_create(panel);
    lv_label_set_text(lbl_title, title);
    lv_obj_set_style_text_color(lbl_title, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl_title, &iosevka_16, 0);
    lv_obj_set_width(lbl_title, 330);
    lv_label_set_long_mode(lbl_title, LV_LABEL_LONG_WRAP);

    // =========================================================================
    // HINT SECTION — shown first so it's always reachable without scrolling
    // Hidden by default (geocaching spoiler), tap header to reveal/re-hide
    // =========================================================================
    if (wp.hint[0]) {
        lv_obj_t* lbl_hint_hdr = lv_label_create(panel);
        lv_label_set_text(lbl_hint_hdr, "HINT  (tap to reveal)");
        lv_obj_set_style_text_color(lbl_hint_hdr, lv_color_hex(0xFFAA00), 0);
        lv_obj_set_style_text_font(lbl_hint_hdr, &iosevka_16, 0);
        lv_obj_set_width(lbl_hint_hdr, 330);
        lv_obj_add_flag(lbl_hint_hdr, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t* lbl_hint = lv_label_create(panel);
        lv_label_set_text(lbl_hint, wp.hint);
        lv_obj_set_style_text_color(lbl_hint, lv_color_hex(0xFFCC66), 0);
        lv_obj_set_style_text_font(lbl_hint, &iosevka_16, 0);
        lv_obj_set_width(lbl_hint, 330);
        lv_label_set_long_mode(lbl_hint, LV_LABEL_LONG_WRAP);
        lv_obj_add_flag(lbl_hint, LV_OBJ_FLAG_HIDDEN);

        lv_obj_add_event_cb(lbl_hint_hdr, [](lv_event_t* e) {
            lv_obj_t* hint = (lv_obj_t*)lv_event_get_user_data(e);
            lv_obj_t* hdr  = (lv_obj_t*)lv_event_get_target(e);
            if (lv_obj_has_flag(hint, LV_OBJ_FLAG_HIDDEN)) {
                lv_obj_clear_flag(hint, LV_OBJ_FLAG_HIDDEN);
                lv_label_set_text(hdr, "HINT  (tap to hide)");
            } else {
                lv_obj_add_flag(hint, LV_OBJ_FLAG_HIDDEN);
                lv_label_set_text(hdr, "HINT  (tap to reveal)");
            }
        }, LV_EVENT_CLICKED, lbl_hint);
    }

    // =========================================================================
    // DESCRIPTION SECTION — below hint, scroll down to read
    // =========================================================================
    if (wp.desc[0]) {
        lv_obj_t* lbl_desc_hdr = lv_label_create(panel);
        lv_label_set_text(lbl_desc_hdr, "DESCRIPTION");
        lv_obj_set_style_text_color(lbl_desc_hdr, lv_color_hex(0x00AA00), 0);
        lv_obj_set_style_text_font(lbl_desc_hdr, &iosevka_16, 0);
        lv_obj_set_width(lbl_desc_hdr, 330);

        lv_obj_t* lbl_desc = lv_label_create(panel);
        lv_label_set_text(lbl_desc, wp.desc);
        lv_obj_set_style_text_color(lbl_desc, lv_color_hex(0xCCCCCC), 0);
        lv_obj_set_style_text_font(lbl_desc, &iosevka_16, 0);
        lv_obj_set_width(lbl_desc, 330);
        lv_label_set_long_mode(lbl_desc, LV_LABEL_LONG_WRAP);
    }

    // Bottom spacer so content doesn't hide behind the circular edge when scrolled
    lv_obj_t* spacer = lv_obj_create(panel);
    lv_obj_set_size(spacer, 1, 80);  // Extra room so last line scrolls clear of the close button
    lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(spacer, 0, 0);
    lv_obj_clear_flag(spacer, LV_OBJ_FLAG_CLICKABLE);

    // =========================================================================
    // BOTTOM BUTTONS — two buttons side-by-side at y=425
    // At y=461 (bottom of 36px button), safe horizontal range is ~156–324px.
    // Left button: x=155 (156–235), Right button: x=245 (245–325) — both within circle.
    // =========================================================================
    bool already_fixed = (ui.fixed_waypoint_index == idx);

    // "X Back" button (left, red)
    lv_obj_t* btn_close = lv_btn_create(ui.screen_waypoint);
    lv_obj_set_size(btn_close, 80, 36);
    lv_obj_set_pos(btn_close, 155, 425);
    lv_obj_set_style_bg_color(btn_close, lv_color_hex(0xFF0000), 0);
    lv_obj_set_style_border_width(btn_close, 0, 0);
    lv_obj_set_style_radius(btn_close, 10, 0);
    lv_obj_t* lbl_x = lv_label_create(btn_close);
    lv_label_set_text(lbl_x, "X Back");
    lv_obj_set_style_text_color(lbl_x, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl_x, &iosevka_16, 0);
    lv_obj_center(lbl_x);
    lv_obj_add_event_cb(btn_close, [](lv_event_t* e) {
        if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
            Serial.println("[WPT_SCREEN] Close button — returning to radar");
            navigation::goToRadarScreen();
        }
    }, LV_EVENT_CLICKED, nullptr);

    // "Fix / Unfix" button (right, green / gray)
    lv_obj_t* btn_fix = lv_btn_create(ui.screen_waypoint);
    lv_obj_set_size(btn_fix, 80, 36);
    lv_obj_set_pos(btn_fix, 245, 425);
    lv_obj_set_style_bg_color(btn_fix, already_fixed ? lv_color_hex(0x444444) : lv_color_hex(0x008800), 0);
    lv_obj_set_style_border_width(btn_fix, 0, 0);
    lv_obj_set_style_radius(btn_fix, 10, 0);
    lv_obj_t* lbl_fix = lv_label_create(btn_fix);
    lv_label_set_text(lbl_fix, already_fixed ? "Unfix" : "Fix");
    lv_obj_set_style_text_color(lbl_fix, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl_fix, &iosevka_16, 0);
    lv_obj_center(lbl_fix);
    // Pass waypoint index as user data (int cast to pointer)
    lv_obj_add_event_cb(btn_fix, [](lv_event_t* e) {
        if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
            ui_manager::UIState& ui = ui_manager::getUIState();
            int idx = (int)(intptr_t)lv_event_get_user_data(e);
            if (ui.fixed_waypoint_index == idx) {
                // Already fixed — toggle off
                ui.fixed_waypoint_index = -1;
                Serial.printf("[WPT_SCREEN] Waypoint %d unfixed\n", idx);
            } else {
                // Fix this waypoint
                ui.fixed_waypoint_index = idx;
                Serial.printf("[WPT_SCREEN] Waypoint %d fixed for proximity sonar\n", idx);
            }
            navigation::goToRadarScreen();
        }
    }, LV_EVENT_CLICKED, (void*)(intptr_t)idx);

    // =========================================================================
    // LOAD THE SCREEN
    // =========================================================================
    lv_scr_load(ui.screen_waypoint);
    Serial.println("[WPT_SCREEN] Screen loaded");
}

} // namespace waypoint_screen
