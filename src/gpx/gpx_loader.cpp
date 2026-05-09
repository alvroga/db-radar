#include "gpx/gpx_loader.h"
#include "ui/ui_manager.h"
#include "utils/task_manager.h"
#include <sys/stat.h>
#include <dirent.h>
#include <stdio.h>
#include <unistd.h>

namespace gpx_loader {

// Configuration
static const char* GPX_FOLDER = "/sdcard/gpx";

// Statistics
static int files_loaded = 0;
static int waypoints_loaded = 0;
static bool was_truncated = false;

// Helper function forward declaration
static bool extractAttribute(const char* tag_content, const char* attr_name, char* output, size_t output_size);

bool init() {
    Serial.println("[GPX_LOADER] Initializing...");

    struct stat st;
    if (stat(GPX_FOLDER, &st) != 0) {
        Serial.println("[GPX_LOADER] Creating /sdcard/gpx folder...");
        if (mkdir(GPX_FOLDER, 0777) != 0) {
            Serial.println("[GPX_LOADER] ERROR: Failed to create gpx folder");
            return false;
        }
    }

    Serial.println("[GPX_LOADER] Initialization complete");
    return true;
}

int loadAllGPXFiles() {
    Serial.println("[GPX_LOADER] Loading all GPX files from /gpx/...");

    // Clear existing waypoints
    clearWaypoints();

    // Open GPX folder
    DIR* dir = opendir(GPX_FOLDER);
    if (!dir) {
        Serial.println("[GPX_LOADER] ERROR: Failed to open gpx folder");
        return 0;
    }

    // Scan for .gpx files
    files_loaded = 0;
    waypoints_loaded = 0;
    was_truncated = false;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_type == DT_REG) {
            const char* fname = entry->d_name;
            size_t nl = strlen(fname);
            if (nl >= 4 && strcasecmp(fname + nl - 4, ".gpx") == 0) {
                char filepath[256];
                snprintf(filepath, sizeof(filepath), "%s/%s", GPX_FOLDER, fname);
                Serial.printf("[GPX_LOADER] Found GPX file: %s\n", fname);

                int parsed = parseGPXFile(filepath);
                if (parsed > 0) {
                    files_loaded++;
                    Serial.printf("[GPX_LOADER] Loaded %d waypoints from %s\n", parsed, fname);
                }
            }
        }
    }
    closedir(dir);

    Serial.printf("[GPX_LOADER] Loading complete: %d files, %d waypoints%s\n",
                  files_loaded, waypoints_loaded, was_truncated ? " (truncated)" : "");

    return waypoints_loaded;
}

int refreshGPXFiles() {
    Serial.println("[GPX_LOADER] Refreshing GPX files...");
    return loadAllGPXFiles();
}

// Strip HTML tags and decode XML entities from src into dst (max_dst includes NUL)
static void stripHtml(const char* src, char* dst, int max_dst) {
    int oi = 0;
    bool in_tag = false;
    for (int i = 0; src[i] && oi < max_dst - 1; i++) {
        // &lt; / &gt; are XML-escaped < and >, which may wrap HTML tags
        if (strncmp(&src[i], "&lt;", 4) == 0) { in_tag = true; i += 3; continue; }
        if (strncmp(&src[i], "&gt;", 4) == 0) {
            in_tag = false; i += 3;
            if (oi > 0 && dst[oi-1] != ' ') dst[oi++] = ' ';
            continue;
        }
        if (src[i] == '<') { in_tag = true; continue; }
        if (src[i] == '>') {
            in_tag = false;
            if (oi > 0 && dst[oi-1] != ' ') dst[oi++] = ' ';
            continue;
        }
        if (in_tag) continue;
        // Decode remaining entities
        if (strncmp(&src[i], "&amp;",  5) == 0) { dst[oi++] = '&';  i += 4; continue; }
        if (strncmp(&src[i], "&apos;", 6) == 0) { dst[oi++] = '\''; i += 5; continue; }
        if (strncmp(&src[i], "&quot;", 6) == 0) { dst[oi++] = '"';  i += 5; continue; }
        if (strncmp(&src[i], "&nbsp;", 6) == 0) { dst[oi++] = ' ';  i += 5; continue; }
        dst[oi++] = src[i];
    }
    // Trim trailing space
    while (oi > 0 && dst[oi-1] == ' ') oi--;
    dst[oi] = '\0';
}

// Extract content after first '>' on the line (handles tags with attributes)
// Returns pointer to content start, or nullptr if tag not on this line
static const char* contentAfterTag(const char* line, const char* tag_prefix) {
    const char* t = strstr(line, tag_prefix);
    if (!t) return nullptr;
    const char* gt = strchr(t, '>');
    if (!gt) return nullptr;
    return gt + 1;
}

// Append stripped content to dst, respecting remaining capacity
static void appendStripped(char* dst, int dst_size, const char* src) {
    int used = strlen(dst);
    int remaining = dst_size - used - 1;
    if (remaining <= 0) return;
    // Add separator if something already in dst
    if (used > 0 && dst[used-1] != ' ' && dst[used-1] != '\n') {
        dst[used++] = ' ';
        remaining--;
    }
    char tmp[512];
    stripHtml(src, tmp, sizeof(tmp));
    strncat(dst, tmp, remaining);
}

int parseGPXFile(const char* filepath) {
    FILE* file = fopen(filepath, "rb");
    if (!file) {
        Serial.printf("[GPX_LOADER] ERROR: Failed to open %s\n", filepath);
        return 0;
    }

    int waypoints_parsed = 0;
    ui_manager::UIState& ui = ui_manager::getUIState();

    // Stateful line-by-line parser — tracks context inside <wpt>...</wpt>
    char line[512];  // Larger buffer for geocaching lines (URLs in descriptions)
    int line_len = 0;

    // Per-waypoint state
    bool in_wpt = false;
    char wpt_lat[32] = {0};
    char wpt_lon[32] = {0};
    char wp_name[48] = {0};
    char wp_display_name[64] = {0};
    char wp_desc[1024] = {0};
    char wp_hint[256] = {0};
    bool in_short_desc = false;
    bool in_long_desc = false;

    int _ch;
    while ((_ch = fgetc(file)) != EOF && waypoints_loaded < ui_manager::RadarConfig::MAX_WAYPOINTS) {
        char c = (char)_ch;

        if (c == '\n' || c == '\r' || line_len >= 511) {
            line[line_len] = '\0';

            if (strstr(line, "<wpt") && strstr(line, "lat=")) {
                // New waypoint — reset state
                in_wpt = true;
                in_short_desc = false;
                in_long_desc = false;
                memset(wpt_lat, 0, sizeof(wpt_lat));
                memset(wpt_lon, 0, sizeof(wpt_lon));
                memset(wp_name, 0, sizeof(wp_name));
                memset(wp_display_name, 0, sizeof(wp_display_name));
                memset(wp_desc, 0, sizeof(wp_desc));
                memset(wp_hint, 0, sizeof(wp_hint));
                extractAttribute(line, "lat", wpt_lat, sizeof(wpt_lat));
                extractAttribute(line, "lon", wpt_lon, sizeof(wpt_lon));

            } else if (in_wpt) {

                if (strstr(line, "</wpt>")) {
                    // Commit waypoint
                    double lat = atof(wpt_lat);
                    double lon = atof(wpt_lon);
                    if (lat >= -90.0 && lat <= 90.0 && lon >= -180.0 && lon <= 180.0
                            && strlen(wpt_lat) > 0 && strlen(wpt_lon) > 0) {

                        bool mx = (task_manager::ui_state_mutex != nullptr) &&
                                  (xSemaphoreTake(task_manager::ui_state_mutex, pdMS_TO_TICKS(100)) == pdTRUE);

                        ui_manager::Waypoint& wp = ui.waypoints[waypoints_loaded];
                        wp.lat = lat;
                        wp.lon = lon;
                        wp.valid = true;
                        // display_name: prefer groundspeak:name, else <name>
                        const char* dname = strlen(wp_display_name) > 0 ? wp_display_name : wp_name;
                        // If desc was truncated (buffer nearly full), clean up at sentence/word boundary
                        int desc_len = strlen(wp_desc);
                        if (desc_len >= (int)sizeof(wp_desc) - 10) {
                            // Search last 200 chars for sentence end
                            int search = desc_len - 200;
                            if (search < 0) search = 0;
                            int cut = -1;
                            for (int si = desc_len - 1; si >= search; si--) {
                                if (wp_desc[si] == '.' || wp_desc[si] == '!' || wp_desc[si] == '?') {
                                    cut = si + 1; break;
                                }
                            }
                            if (cut > 0) {
                                wp_desc[cut] = '\0';
                            } else {
                                // Fall back to last word boundary
                                while (desc_len > 0 && wp_desc[desc_len-1] != ' ') desc_len--;
                                if (desc_len > 0) wp_desc[desc_len] = '\0';
                            }
                            strncat(wp_desc, " ...", sizeof(wp_desc) - strlen(wp_desc) - 1);
                        }
                        strncpy(wp.name,         wp_name,         sizeof(wp.name) - 1);
                        strncpy(wp.display_name, dname,           sizeof(wp.display_name) - 1);
                        strncpy(wp.desc,         wp_desc,         sizeof(wp.desc) - 1);
                        strncpy(wp.hint,         wp_hint,         sizeof(wp.hint) - 1);
                        waypoints_loaded++;
                        ui.waypoint_count = waypoints_loaded;

                        if (mx) xSemaphoreGive(task_manager::ui_state_mutex);

                        waypoints_parsed++;
                        Serial.printf("[GPX_LOADER]   Waypoint %d: '%s' (%.6f, %.6f)\n",
                                      waypoints_loaded, dname, lat, lon);
                    }
                    in_wpt = false;

                } else if (in_short_desc) {
                    if (strstr(line, "</groundspeak:short_description>") ||
                        strstr(line, "</groundspeak:short_desc")) {
                        in_short_desc = false;
                    } else {
                        appendStripped(wp_desc, sizeof(wp_desc), line);
                    }

                } else if (in_long_desc) {
                    if (strstr(line, "</groundspeak:long_description>") ||
                        strstr(line, "</groundspeak:long_desc")) {
                        in_long_desc = false;
                    } else {
                        appendStripped(wp_desc, sizeof(wp_desc), line);
                    }

                } else if (strstr(line, "<name>") && !strstr(line, "groundspeak")) {
                    const char* c_start = strstr(line, "<name>");
                    if (c_start) {
                        c_start += 6;
                        const char* c_end = strstr(c_start, "</name>");
                        if (c_end) {
                            int len = (int)(c_end - c_start);
                            if (len >= (int)sizeof(wp_name)) len = sizeof(wp_name) - 1;
                            strncpy(wp_name, c_start, len);
                            wp_name[len] = '\0';
                        }
                    }

                } else if (strstr(line, "<groundspeak:name>")) {
                    const char* c_start = strstr(line, "<groundspeak:name>");
                    if (c_start) {
                        c_start += 18;
                        const char* c_end = strstr(c_start, "</groundspeak:name>");
                        if (c_end) {
                            int len = (int)(c_end - c_start);
                            if (len >= (int)sizeof(wp_display_name)) len = sizeof(wp_display_name) - 1;
                            strncpy(wp_display_name, c_start, len);
                            wp_display_name[len] = '\0';
                        }
                    }

                } else if (strstr(line, "<groundspeak:short_description")) {
                    const char* content = contentAfterTag(line, "<groundspeak:short_description");
                    if (content && strlen(content) > 0) {
                        const char* close = strstr(content, "</groundspeak:short_desc");
                        if (close) {
                            // Single-line content
                            char tmp[512];
                            int len = (int)(close - content);
                            if (len >= (int)sizeof(tmp)) len = sizeof(tmp) - 1;
                            strncpy(tmp, content, len); tmp[len] = '\0';
                            appendStripped(wp_desc, sizeof(wp_desc), tmp);
                        } else {
                            appendStripped(wp_desc, sizeof(wp_desc), content);
                            in_short_desc = true;
                        }
                    } else {
                        in_short_desc = true;
                    }

                } else if (strstr(line, "<groundspeak:long_description")) {
                    const char* content = contentAfterTag(line, "<groundspeak:long_description");
                    if (content && strlen(content) > 0) {
                        const char* close = strstr(content, "</groundspeak:long_desc");
                        if (close) {
                            char tmp[512];
                            int len = (int)(close - content);
                            if (len >= (int)sizeof(tmp)) len = sizeof(tmp) - 1;
                            strncpy(tmp, content, len); tmp[len] = '\0';
                            appendStripped(wp_desc, sizeof(wp_desc), tmp);
                        } else {
                            appendStripped(wp_desc, sizeof(wp_desc), content);
                            in_long_desc = true;
                        }
                    } else {
                        in_long_desc = true;
                    }

                } else if (strstr(line, "<groundspeak:encoded_hints>")) {
                    const char* c_start = strstr(line, "<groundspeak:encoded_hints>");
                    if (c_start) {
                        c_start += 27;
                        const char* c_end = strstr(c_start, "</groundspeak:encoded_hints>");
                        if (c_end) {
                            int len = (int)(c_end - c_start);
                            if (len >= (int)sizeof(wp_hint)) len = sizeof(wp_hint) - 1;
                            strncpy(wp_hint, c_start, len);
                            wp_hint[len] = '\0';
                        }
                    }

                // Standard GPX tags — fallbacks used when groundspeak tags are absent
                } else if (strstr(line, "<desc>") && !strstr(line, "groundspeak") && strlen(wp_desc) == 0) {
                    const char* c_start = strstr(line, "<desc>");
                    if (c_start) {
                        c_start += 6;
                        const char* c_end = strstr(c_start, "</desc>");
                        if (c_end) {
                            char tmp[512];
                            int len = (int)(c_end - c_start);
                            if (len >= (int)sizeof(tmp)) len = sizeof(tmp) - 1;
                            strncpy(tmp, c_start, len); tmp[len] = '\0';
                            appendStripped(wp_desc, sizeof(wp_desc), tmp);
                        }
                    }

                } else if (strstr(line, "<cmt>") && strlen(wp_hint) == 0) {
                    const char* c_start = strstr(line, "<cmt>");
                    if (c_start) {
                        c_start += 5;
                        const char* c_end = strstr(c_start, "</cmt>");
                        if (c_end) {
                            int len = (int)(c_end - c_start);
                            if (len >= (int)sizeof(wp_hint)) len = sizeof(wp_hint) - 1;
                            strncpy(wp_hint, c_start, len);
                            wp_hint[len] = '\0';
                        }
                    }
                }
            }

            line_len = 0;
        } else {
            line[line_len++] = c;
        }
    }

    // Check if we hit the limit
    if (waypoints_loaded >= ui_manager::RadarConfig::MAX_WAYPOINTS && !feof(file)) {
        was_truncated = true;
        Serial.printf("[GPX_LOADER] WARNING: Waypoint limit reached (%d), file parsing truncated\n",
                      ui_manager::RadarConfig::MAX_WAYPOINTS);
    }

    fclose(file);
    return waypoints_parsed;
}

int getWaypointCount() {
    return waypoints_loaded;
}

void clearWaypoints() {
    Serial.println("[GPX_LOADER] Clearing all waypoints...");

    // CRITICAL: Protect waypoint array clear with mutex
    bool mutex_acquired = false;
    if (task_manager::ui_state_mutex != nullptr) {
        mutex_acquired = (xSemaphoreTake(task_manager::ui_state_mutex, pdMS_TO_TICKS(100)) == pdTRUE);
    }

    ui_manager::UIState& ui = ui_manager::getUIState();

    // Clear count first to prevent UI from reading stale waypoints
    ui.waypoint_count = 0;

    for (int i = 0; i < ui_manager::RadarConfig::MAX_WAYPOINTS; i++) {
        ui.waypoints[i].lat = 0.0;
        ui.waypoints[i].lon = 0.0;
        ui.waypoints[i].valid = false;
    }

    if (mutex_acquired) {
        xSemaphoreGive(task_manager::ui_state_mutex);
    }

    waypoints_loaded = 0;
    files_loaded = 0;
    was_truncated = false;
}

void getStats(int& file_count, int& waypoint_count, bool& truncated) {
    file_count = files_loaded;
    waypoint_count = waypoints_loaded;
    truncated = was_truncated;
}

// Helper function: Extract XML attribute value
// Example: extractAttribute("lat=\"37.123\" lon=\"-122.456\"", "lat", output, size)
// Returns true and sets output to "37.123"
bool extractAttribute(const char* tag_content, const char* attr_name, char* output, size_t output_size) {
    // Find attribute name
    const char* attr_start = strstr(tag_content, attr_name);
    if (!attr_start) return false;

    // Find opening quote
    const char* quote_open = strchr(attr_start, '\"');
    if (!quote_open) return false;
    quote_open++; // Move past quote

    // Find closing quote
    const char* quote_close = strchr(quote_open, '\"');
    if (!quote_close) return false;

    // Calculate length
    size_t len = quote_close - quote_open;
    if (len >= output_size) len = output_size - 1;

    // Copy value
    strncpy(output, quote_open, len);
    output[len] = '\0';

    return true;
}

} // namespace gpx_loader
