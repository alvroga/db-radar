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
 * @param waypoint_count Output: total waypoints loaded
 * @param truncated Output: true if waypoints were truncated due to MAX_WAYPOINTS limit
 */
void getStats(int& file_count, int& waypoint_count, bool& truncated);

} // namespace gpx_loader

#endif // GPX_LOADER_H
