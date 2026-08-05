#ifndef GPX_LOADER_H
#define GPX_LOADER_H

#include "core/arduino_compat.h"

namespace gpx_loader {

/**
 * @brief Initialize GPX loader system
 * @return true on success, false on failure
 */
bool init();

/**
 * @brief Load all GPX files from /gpx/ folder and parse waypoints
 * @return Number of waypoints loaded (up to MAX_WAYPOINTS)
 */
int loadAllGPXFiles();

/**
 * @brief Refresh waypoints by re-scanning /gpx/ folder
 * @return Number of waypoints loaded
 */
int refreshGPXFiles();

/**
 * @brief Parse a single GPX file and extract waypoints
 * @param filepath Full path to GPX file
 * @return Number of waypoints parsed from this file
 */
int parseGPXFile(const char* filepath);

/**
 * @brief Get total count of loaded waypoints
 * @return Number of currently loaded waypoints
 */
int getWaypointCount();

/**
 * @brief Clear all loaded waypoints
 */
void clearWaypoints();

/**
 * @brief Get statistics about loaded GPX files
 * @param file_count Output: number of GPX files found
 * @param waypoint_count Output: total waypoints loaded (working-set size)
 * @param truncated Output: true if the full PSRAM index itself overflowed
 *        (rare, real data loss) — distinct from working_set_capped below.
 */
void getStats(int& file_count, int& waypoint_count, bool& truncated);

/**
 * @brief Two-tier index stats — see docs/waypoint_two_tier_index_plan.md / ADR-0023.
 * @param index_count Output: total waypoints known across all GPX files (PSRAM index)
 * @param index_truncated Output: true only if index_count hit gpx_index::MAX_INDEX_ENTRIES
 * @param working_set_capped Output: true if index_count > MAX_WAYPOINTS (expected/normal,
 *        not a warning — it just means the working set is the closest MAX_WAYPOINTS, not everything)
 */
void getIndexStats(int& index_count, bool& index_truncated, bool& working_set_capped);

/**
 * @brief Re-run closest-N selection against a new center without rescanning files.
 * Used by the movement-triggered reselect (task_manager.cpp). No-op (returns
 * current working-set size unchanged) if the PSRAM index isn't available.
 * @return Number of waypoints in the working set after selection
 */
int reselect(double center_lat, double center_lon);

/**
 * @brief Write-through found-state for a working-set slot into the PSRAM
 * index entry it was materialized from, so it survives that slot being
 * recycled by a future reselect(). Call whenever ui.waypoints[slot].found
 * is set directly.
 */
void markWaypointFound(int slot, bool found);

} // namespace gpx_loader

#endif // GPX_LOADER_H
