#include "gpx/gpx_parser.h"
#include <sys/stat.h>
#include <stdio.h>

namespace gpx_parser {

// Parsing statistics
static int last_waypoint_count = 0;
static int last_error_count = 0;

/**
 * @brief Extract attribute value from XML tag
 * Example: lat="34.133417" -> returns "34.133417"
 */
static String extractAttribute(const String& xml, const char* attr_name, int start_pos) {
    String search_str = String(attr_name) + "=\"";
    int attr_start = xml.indexOf(search_str, start_pos);

    if (attr_start == -1) {
        return "";
    }

    attr_start += search_str.length();
    int attr_end = xml.indexOf("\"", attr_start);

    if (attr_end == -1) {
        return "";
    }

    return xml.substring(attr_start, attr_end);
}

/**
 * @brief Extract content between XML tags
 * Example: <name>Home</name> -> returns "Home"
 */
static String extractTagContent(const String& xml, const char* tag_name, int start_pos) {
    String open_tag = String("<") + tag_name + ">";
    String close_tag = String("</") + tag_name + ">";

    int content_start = xml.indexOf(open_tag, start_pos);
    if (content_start == -1) {
        return "";
    }

    content_start += open_tag.length();
    int content_end = xml.indexOf(close_tag, content_start);

    if (content_end == -1) {
        return "";
    }

    return xml.substring(content_start, content_end);
}

std::vector<Waypoint> parseGPX(const String& gpx_data) {
    std::vector<Waypoint> waypoints;
    last_waypoint_count = 0;
    last_error_count = 0;

    Serial.println("[GPX_PARSER] Starting GPX parse...");

    // Find all <wpt> tags
    int search_pos = 0;
    while (true) {
        int wpt_start = gpx_data.indexOf("<wpt", search_pos);
        if (wpt_start == -1) {
            break;  // No more waypoints
        }

        int wpt_end = gpx_data.indexOf("</wpt>", wpt_start);
        if (wpt_end == -1) {
            Serial.println("[GPX_PARSER] ERROR: Unclosed <wpt> tag");
            last_error_count++;
            break;
        }

        // Extract waypoint data
        Waypoint wp;

        // Extract lat attribute
        String lat_str = extractAttribute(gpx_data, "lat", wpt_start);
        if (lat_str.length() > 0) {
            wp.lat = lat_str.toDouble();
        } else {
            Serial.println("[GPX_PARSER] ERROR: Missing lat attribute");
            last_error_count++;
            search_pos = wpt_end + 6;
            continue;
        }

        // Extract lon attribute
        String lon_str = extractAttribute(gpx_data, "lon", wpt_start);
        if (lon_str.length() > 0) {
            wp.lon = lon_str.toDouble();
        } else {
            Serial.println("[GPX_PARSER] ERROR: Missing lon attribute");
            last_error_count++;
            search_pos = wpt_end + 6;
            continue;
        }

        // Extract name (optional)
        wp.name = extractTagContent(gpx_data, "name", wpt_start);
        if (wp.name.length() == 0) {
            // Generate default name
            wp.name = "Waypoint " + String(waypoints.size() + 1);
        }

        // Validate coordinates (basic sanity check)
        if (wp.lat < -90.0 || wp.lat > 90.0 || wp.lon < -180.0 || wp.lon > 180.0) {
            Serial.printf("[GPX_PARSER] ERROR: Invalid coordinates: lat=%.6f, lon=%.6f\n",
                          wp.lat, wp.lon);
            last_error_count++;
            search_pos = wpt_end + 6;
            continue;
        }

        waypoints.push_back(wp);
        last_waypoint_count++;

        Serial.printf("[GPX_PARSER] Parsed waypoint: '%s' (%.6f, %.6f)\n",
                      wp.name.c_str(), wp.lat, wp.lon);

        // Move to next waypoint
        search_pos = wpt_end + 6;
    }

    Serial.printf("[GPX_PARSER] Parse complete: %d waypoints, %d errors\n",
                  last_waypoint_count, last_error_count);

    return waypoints;
}

std::vector<Waypoint> parseGPXFile(const char* filepath) {
    Serial.printf("[GPX_PARSER] Reading file: %s\n", filepath);

    FILE* file = fopen(filepath, "rb");
    if (!file) {
        Serial.printf("[GPX_PARSER] ERROR: Failed to open file: %s\n", filepath);
        return std::vector<Waypoint>();
    }

    // Check file size
    struct stat st;
    size_t file_size = 0;
    if (stat(filepath, &st) == 0) file_size = (size_t)st.st_size;

    if (file_size == 0) {
        Serial.println("[GPX_PARSER] ERROR: File is empty");
        fclose(file);
        return std::vector<Waypoint>();
    }

    if (file_size > 100000) {
        Serial.printf("[GPX_PARSER] WARNING: Large file (%d bytes), may take time to parse\n",
                      (int)file_size);
    }

    // Read entire file into String
    String gpx_data;
    gpx_data.reserve(file_size + 1);
    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), file)) > 0) {
        buf[n] = '\0';
        gpx_data += buf;
    }
    fclose(file);

    Serial.printf("[GPX_PARSER] Read %d bytes from file\n", (int)gpx_data.length());

    // Parse the GPX data
    return parseGPX(gpx_data);
}

void getLastParseStats(int& waypoint_count, int& error_count) {
    waypoint_count = last_waypoint_count;
    error_count = last_error_count;
}

} // namespace gpx_parser
