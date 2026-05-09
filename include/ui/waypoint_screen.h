#ifndef WAYPOINT_SCREEN_H
#define WAYPOINT_SCREEN_H

namespace waypoint_screen {

/**
 * @brief Open the waypoint detail screen for the currently selected waypoint.
 * Uses ui_manager::getUIState().selected_waypoint_index.
 * Destroys any previously created waypoint screen first.
 * Call only from the UI Task (LVGL thread).
 */
void open();

/**
 * @brief Destroy the waypoint detail screen and free its LVGL objects.
 * Called automatically when navigating away.
 */
void close();

} // namespace waypoint_screen

#endif // WAYPOINT_SCREEN_H
