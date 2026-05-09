#ifndef GPX_PARSER_H
#define GPX_PARSER_H

#include "core/arduino_compat.h"
#include <vector>

namespace gpx_parser {

/**
 * @brief Waypoint data extracted from GPX file
 */
struct Waypoint {
    double lat;           // Latitude in decimal degrees
    double lon;           // Longitude in decimal degrees
    String name;          // Waypoint name (from <name> tag)

    Waypoint() : lat(0), lon(0), name("") {}
    Waypoint(double lat_, double lon_, const String& name_)
        : lat(lat_), lon(lon_), name(name_) {}
};

/**
 * @brief Parse GPX XML data and extract waypoints
 *
 * This parser handles standard GPX format waypoints:
 * <wpt lat="34.133417" lon="-118.145190">
 *   <name>Home</name>
 * </wpt>
 *
 * @param gpx_data GPX file content as String
 * @return Vector of waypoints extracted from the file
 */
std::vector<Waypoint> parseGPX(const String& gpx_data);

/**
 * @brief Parse GPX file from SD card
 *
 * @param filepath Full path to GPX file on SD card (e.g., "/gpx/route.gpx")
 * @return Vector of waypoints, empty if file read fails
 */
std::vector<Waypoint> parseGPXFile(const char* filepath);

/**
 * @brief Get parsing statistics from last parse operation
 *
 * @param waypoint_count Output: number of waypoints found
 * @param error_count Output: number of parsing errors encountered
 */
void getLastParseStats(int& waypoint_count, int& error_count);

} // namespace gpx_parser

#endif // GPX_PARSER_H
