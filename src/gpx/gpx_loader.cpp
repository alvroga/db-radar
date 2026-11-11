#include "gpx/gpx_loader.h"
#include "gpx/gpx_index.h"
#include "ui/ui_manager.h"
#include "utils/task_manager.h"
#include "utils/geo.h"
#include "esp_heap_caps.h"
#include <sys/stat.h>
#include <dirent.h>
#include <stdio.h>
#include <unistd.h>
#include <algorithm>
#include <math.h>
#include "esp_timer.h"

namespace gpx_loader {

// Configuration — primary storage moved SD -> FFat (see ADR-0024): FFat isn't behind
// the enclosure's disassembly-required SD card, and GPX capacity was never SD's job
// to begin with (the 8,192-entry PSRAM index caps usage well before flash bytes do).
static const char* GPX_FOLDER = "/ffat/gpx";

// Statistics
static int files_loaded = 0;
static int waypoints_loaded = 0;          // working-set size (== ui.waypoint_count)
static bool was_truncated = false;        // legacy path: MAX_WAYPOINTS hit; indexed path: index overflow
static bool working_set_capped = false;   // indexed path only: index bigger than the working set

// True once gpx_index::init() succeeded — selects the two-tier path vs. the
// single-pass legacy fallback (see init()).
static bool g_index_available = false;

// distance/entry_idx scratch for closest-N selection, allocated once in
// PSRAM (see plan §3). Indexed 1:1 with gpx_index entries.
struct DistEntry { float dist; int idx; };  // dist = squared equirectangular degrees^2 (ranking only, not metres)
static DistEntry* g_scratch = nullptr;

// slot -> index-entry id for the current working set, so found-state and a
// future delta-reselect can map back to the PSRAM index. -1 = empty/unset.
static int g_slot_source[ui_manager::RadarConfig::MAX_WAYPOINTS];

// Closest-N ranking. Only ordering matters here (never displayed), so use a
// float squared equirectangular distance instead of per-entry double haversine,
// and nth_element (O(n)) instead of partial_sort. Slot order downstream is
// irrelevant (see selectAndMaterialize). Near the selection boundary, entries
// whose true distances differ by well under the equirectangular error may
// swap vs. a haversine oracle; at working-set radii this is sub-metre.
static void rankByDistance(double clat, double clon, int n) {
    const float clat_f = (float)clat, clon_f = (float)clon;
    const float kx = cosf((float)(clat * M_PI / 180.0));
    for (int i = 0; i < n; i++) {
        const gpx_index::IndexEntry& e = gpx_index::getEntry(i);
        float dy = e.lat - clat_f;
        float dx = e.lon - clon_f;
        if (dx > 180.0f) dx -= 360.0f; else if (dx < -180.0f) dx += 360.0f;
        dx *= kx;
        g_scratch[i].dist = dx * dx + dy * dy;
        g_scratch[i].idx = i;
    }
}

static void selectClosest(int want, int n) {
    if (want < n) {
        std::nth_element(g_scratch, g_scratch + want, g_scratch + n,
                         [](const DistEntry& a, const DistEntry& b) { return a.dist < b.dist; });
    }
}

// Line reader for the GPX parsers. Ends a line at \n, \r, EOF, when the buffer
// is full (the char that didn't fit starts the next line - nothing is dropped),
// and - so minified single-line GPX works - right after any '>' that is
// immediately followed by '<' (tag boundary). Every parser branch handles one
// field per line, which is exactly what this yields for minified files.
// Tracks the byte offset of each line start itself (no ftell/ungetc games).
struct LineReader {
    FILE* f; long pos; int pend;
    explicit LineReader(FILE* fp) : f(fp), pos(ftell(fp)), pend(-2) {}
    int get() { if (pend != -2) { int c = pend; pend = -2; return c; } return fgetc(f); }
    // Returns true when EOF was reached (the returned line may be partial/empty).
    bool readLine(char* buf, int cap, long* start) {
        int len = 0;
        *start = pos;
        while (true) {
            int ch = get();
            if (ch == EOF) { buf[len] = '\0'; return true; }
            pos++;
            if (ch == '\n' || ch == '\r') { buf[len] = '\0'; return false; }
            if (len >= cap - 1) { pend = ch; pos--; buf[len] = '\0'; return false; }
            buf[len++] = (char)ch;
            if (ch == '>') {
                int nx = get();
                pend = nx;
                if (nx == '<') { buf[len] = '\0'; return false; }
            }
        }
    }
};

// Helper function forward declarations
static bool extractAttribute(const char* tag_content, const char* attr_name, char* output, size_t output_size);
static void stripHtml(const char* src, char* dst, int max_dst);
static const char* contentAfterTag(const char* line, const char* tag_prefix);
static void appendStripped(char* dst, int dst_size, const char* src);
static int buildFileIndex(const char* filepath, uint16_t file_id);
static int selectAndMaterialize(double center_lat, double center_lon);
static bool parseOneWaypointAt(FILE* file, ui_manager::Waypoint& out_wp);
static int loadAllGPXFilesLegacy();
static int loadAllGPXFilesIndexed(bool show_boot_progress);

bool init() {
    Serial.println("[GPX_LOADER] Initializing...");

    struct stat st;
    if (stat(GPX_FOLDER, &st) != 0) {
        Serial.println("[GPX_LOADER] Creating /ffat/gpx folder...");
        if (mkdir(GPX_FOLDER, 0777) != 0) {
            Serial.println("[GPX_LOADER] ERROR: Failed to create gpx folder");
            return false;
        }
    }

    for (int i = 0; i < ui_manager::RadarConfig::MAX_WAYPOINTS; i++) g_slot_source[i] = -1;

    // Two-tier index is best-effort — PSRAM alloc failure degrades cleanly to
    // the pre-existing single-pass file-order behavior (never crash).
    bool index_ok = gpx_index::init();
    if (index_ok) {
        g_scratch = (DistEntry*)heap_caps_calloc(gpx_index::MAX_INDEX_ENTRIES, sizeof(DistEntry), MALLOC_CAP_SPIRAM);
        if (!g_scratch) {
            Serial.println("[GPX_LOADER] ERROR: PSRAM alloc failed for selection scratch — using legacy loader");
            index_ok = false;
        }
    }
    g_index_available = index_ok;
    Serial.printf("[GPX_LOADER] Two-tier index: %s\n", g_index_available ? "enabled" : "disabled (legacy loader)");

    Serial.println("[GPX_LOADER] Initialization complete");
    return true;
}

int loadAllGPXFiles(bool show_boot_progress) {
    clearWaypoints();
    if (g_index_available) {
        return loadAllGPXFilesIndexed(show_boot_progress);
    }
    return loadAllGPXFilesLegacy();
}

static int loadAllGPXFilesIndexed(bool show_boot_progress) {
    Serial.println("[GPX_LOADER] Loading all GPX files from /gpx/ (indexed)...");

    gpx_index::reset();

    DIR* dir = opendir(GPX_FOLDER);
    if (!dir) {
        Serial.println("[GPX_LOADER] ERROR: Failed to open gpx folder");
        return 0;
    }

    files_loaded = 0;
    uint32_t last_progress_ms = millis();

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        const char* fname = entry->d_name;
        size_t nl = strlen(fname);
        if (nl >= 4 && strcasecmp(fname + nl - 4, ".gpx") == 0) {
            int file_id = gpx_index::addFile(fname);
            if (file_id < 0) {
                Serial.printf("[GPX_LOADER] WARNING: index file table full, skipping %s\n", fname);
                continue;
            }
            char filepath[256];
            snprintf(filepath, sizeof(filepath), "%s/%s", GPX_FOLDER, fname);
            int n = buildFileIndex(filepath, (uint16_t)file_id);
            if (n > 0) {
                files_loaded++;
            } else {
                Serial.printf("[GPX_LOADER] WARNING: %s yielded 0 waypoints\n", fname);
            }

            // Boot-only: pump LVGL so the loading spinner keeps animating and the
            // status label shows real progress instead of sitting frozen for the
            // whole scan (worse now that MAX_INDEX_FILES=1024 makes this loop longer).
            // See the show_boot_progress contract in gpx_loader.h — never true post-boot.
            if (show_boot_progress && (millis() - last_progress_ms >= 150)) {
                last_progress_ms = millis();
                char msg[48];
                snprintf(msg, sizeof(msg), "Loading waypoints... (%d)", files_loaded);
                ui_manager::updateLoadingStatus(msg);
                lv_task_handler();
            }
        }
    }
    closedir(dir);

    was_truncated = gpx_index::wasIndexTruncated();
    int index_count = gpx_index::getEntryCount();
    working_set_capped = index_count > ui_manager::RadarConfig::MAX_WAYPOINTS;

    ui_manager::UIState& ui = ui_manager::getUIState();
    waypoints_loaded = selectAndMaterialize(ui.center_lat, ui.center_lon);

    Serial.printf("[GPX_LOADER] Loading complete: %d files, %d indexed, %d in working set%s%s\n",
                  files_loaded, index_count, waypoints_loaded,
                  was_truncated ? " (index truncated)" : "",
                  working_set_capped ? " (working set capped)" : "");

    return waypoints_loaded;
}

int refreshGPXFiles() {
    Serial.println("[GPX_LOADER] Refreshing GPX files...");
    return loadAllGPXFiles();
}

// Movement-triggered reselect: recompute the closest-N set and touch only
// the slots that actually changed. Slot assignment for entries that remain
// selected is stable across calls (a survivor never moves slots — see the
// "kept" bookkeeping below), which is what lets fixed_waypoint_index survive
// a reselect without needing a lat/lon re-resolve: if its slot's source
// entry is unchanged after the call, the fix is still valid.
int reselect(double center_lat, double center_lon) {
    if (!g_index_available || !g_scratch) return waypoints_loaded;

    int n = gpx_index::getEntryCount();
    ui_manager::UIState& ui = ui_manager::getUIState();
    if (n == 0) {
        waypoints_loaded = selectAndMaterialize(center_lat, center_lon);
        return waypoints_loaded;
    }

    int64_t t_rank0 = esp_timer_get_time();
    rankByDistance(center_lat, center_lon, n);
    int want = n < ui_manager::RadarConfig::MAX_WAYPOINTS ? n : ui_manager::RadarConfig::MAX_WAYPOINTS;
    selectClosest(want, n);
    int64_t rank_us = esp_timer_get_time() - t_rank0;

    // Which of the `want` new winners are already sitting in some slot
    // (kept in place, no reparse) vs. which existing slots are now free.
    static bool new_kept[ui_manager::RadarConfig::MAX_WAYPOINTS];
    static bool slot_kept[ui_manager::RadarConfig::MAX_WAYPOINTS];
    for (int i = 0; i < want; i++) new_kept[i] = false;
    for (int s = 0; s < ui_manager::RadarConfig::MAX_WAYPOINTS; s++) slot_kept[s] = false;

    for (int s = 0; s < ui_manager::RadarConfig::MAX_WAYPOINTS; s++) {
        int old_entry = g_slot_source[s];
        if (old_entry < 0) continue;
        for (int i = 0; i < want; i++) {
            if (!new_kept[i] && g_scratch[i].idx == old_entry) {
                new_kept[i] = true;
                slot_kept[s] = true;
                break;
            }
        }
    }

    // Capture the fixed waypoint's source entry (if any) before mutating
    // slots, so we can tell afterward whether it survived unchanged.
    int fixed_slot = ui.fixed_waypoint_index;
    int fixed_entry_before = (fixed_slot >= 0 && fixed_slot < ui_manager::RadarConfig::MAX_WAYPOINTS)
                             ? g_slot_source[fixed_slot] : -1;

    struct Pending { int entry_idx; };
    static Pending pending[ui_manager::RadarConfig::MAX_WAYPOINTS];
    int pending_count = 0;
    for (int i = 0; i < want; i++) {
        if (!new_kept[i]) pending[pending_count++] = { g_scratch[i].idx };
    }
    std::sort(pending, pending + pending_count, [](const Pending& a, const Pending& b) {
        const gpx_index::IndexEntry& ea = gpx_index::getEntry(a.entry_idx);
        const gpx_index::IndexEntry& eb = gpx_index::getEntry(b.entry_idx);
        if (ea.file_id != eb.file_id) return ea.file_id < eb.file_id;
        return ea.file_offset < eb.file_offset;
    });

    int changed = 0;
    int free_slot_search = 0;
    FILE* file = nullptr;
    uint16_t open_file_id = 0xFFFF;
    char filepath[256];

    for (int p = 0; p < pending_count; p++) {
        while (free_slot_search < ui_manager::RadarConfig::MAX_WAYPOINTS && slot_kept[free_slot_search]) {
            free_slot_search++;
        }
        if (free_slot_search >= ui_manager::RadarConfig::MAX_WAYPOINTS) break;  // shouldn't happen: want <= MAX_WAYPOINTS
        int slot = free_slot_search;
        slot_kept[slot] = true;  // claim it
        free_slot_search++;

        const gpx_index::IndexEntry& e = gpx_index::getEntry(pending[p].entry_idx);
        if (!file || e.file_id != open_file_id) {
            if (file) { fclose(file); file = nullptr; }
            snprintf(filepath, sizeof(filepath), "%s/%s", GPX_FOLDER, gpx_index::getFileName(e.file_id));
            file = fopen(filepath, "rb");
            open_file_id = e.file_id;
            if (!file) {
                Serial.printf("[GPX_LOADER] ERROR: reselect failed to open %s\n", filepath);
                g_slot_source[slot] = -1;
                continue;
            }
        }
        if (fseek(file, (long)e.file_offset, SEEK_SET) != 0) {
            g_slot_source[slot] = -1;
            continue;
        }

        ui_manager::Waypoint& wp = ui.waypoints[slot];
        bool mx = (task_manager::ui_state_mutex != nullptr) &&
                  (xSemaphoreTake(task_manager::ui_state_mutex, pdMS_TO_TICKS(100)) == pdTRUE);
        wp.lat = 0.0;
        wp.lon = 0.0;
        wp.valid = false;
        wp.found = false;
        wp.name[0] = '\0';
        wp.display_name[0] = '\0';
        if (wp.desc) wp.desc[0] = '\0';
        if (wp.hint) wp.hint[0] = '\0';
        if (mx) xSemaphoreGive(task_manager::ui_state_mutex);

        if (parseOneWaypointAt(file, wp)) {
            wp.found = gpx_index::getFound(pending[p].entry_idx);
            g_slot_source[slot] = pending[p].entry_idx;
            changed++;
        } else {
            g_slot_source[slot] = -1;
        }
    }
    if (file) fclose(file);

    // Slots beyond the new `want` (left over from a larger previous
    // selection) are stale — drop their source mapping so they can't be
    // mistaken for live data if `want` ever grows without a fresh reselect.
    for (int s = want; s < ui_manager::RadarConfig::MAX_WAYPOINTS; s++) {
        g_slot_source[s] = -1;
    }

    bool mx2 = (task_manager::ui_state_mutex != nullptr) &&
               (xSemaphoreTake(task_manager::ui_state_mutex, pdMS_TO_TICKS(100)) == pdTRUE);
    ui.waypoint_count = want;

    // fixed_waypoint_index: valid only if its slot's source entry is exactly
    // what it was before this call — any other outcome (evicted, or the slot
    // got recycled for a different entry) means the fix no longer points at
    // a live waypoint.
    if (fixed_slot >= 0) {
        if (fixed_entry_before < 0 || g_slot_source[fixed_slot] != fixed_entry_before) {
            ui.fixed_waypoint_index = -1;
        }
    }
    // The waypoint detail screen (if any) is skipped by the caller before a
    // periodic reselect — see task_manager.cpp — so this is a defensive
    // clear, not expected to fire while it's open.
    ui.selected_waypoint_index = -1;

    if (mx2) xSemaphoreGive(task_manager::ui_state_mutex);

    waypoints_loaded = want;
    if (changed > 0) {
        Serial.printf("[GPX_LOADER] Reselect: %d/%d slots changed (center %.6f, %.6f), rank %lld us over %d entries\n",
                      changed, want, center_lat, center_lon, (long long)rank_us, n);
    }
    return waypoints_loaded;
}

void markWaypointFound(int slot, bool found) {
    if (slot < 0 || slot >= ui_manager::RadarConfig::MAX_WAYPOINTS) return;
    if (!g_index_available) return;
    int entry_idx = g_slot_source[slot];
    if (entry_idx < 0) return;
    gpx_index::setFound(entry_idx, found);
}

// Scan one file for "<wpt lat=... lon=...>" lines only — no field parsing,
// no MAX_WAYPOINTS cap. Records the byte offset at the start of each such
// line so pass 2 (parseOneWaypointAt) can fseek straight to it.
static int buildFileIndex(const char* filepath, uint16_t file_id) {
    FILE* file = fopen(filepath, "rb");
    if (!file) {
        Serial.printf("[GPX_LOADER] ERROR: Failed to open %s for indexing\n", filepath);
        return 0;
    }

    int count = 0;
    char line[512];
    long line_start = 0;
    LineReader lr(file);

    while (true) {
        bool at_eof = lr.readLine(line, sizeof(line), &line_start);
        {

            if (strstr(line, "<wpt") && strstr(line, "lat=")) {
                char lat_s[32] = {0};
                char lon_s[32] = {0};
                extractAttribute(line, "lat", lat_s, sizeof(lat_s));
                extractAttribute(line, "lon", lon_s, sizeof(lon_s));
                double lat = atof(lat_s);
                double lon = atof(lon_s);
                if (strlen(lat_s) > 0 && strlen(lon_s) > 0
                        && lat >= -90.0 && lat <= 90.0 && lon >= -180.0 && lon <= 180.0) {
                    if (gpx_index::addEntry((float)lat, (float)lon, (uint32_t)line_start, file_id)) {
                        count++;
                    }
                }
            }

            if (at_eof) break;
        }
    }

    fclose(file);
    return count;
}

// Haversine-distance every index entry against (center_lat, center_lon),
// partial_sort to the closest MAX_WAYPOINTS, then materialize those winners
// into ui.waypoints[]. Winners are grouped by source file and sorted by
// file_offset within each file to avoid re-opening files or seeking backward.
static int selectAndMaterialize(double center_lat, double center_lon) {
    int n = gpx_index::getEntryCount();
    ui_manager::UIState& ui = ui_manager::getUIState();

    if (n == 0 || !g_scratch) {
        bool mx = (task_manager::ui_state_mutex != nullptr) &&
                  (xSemaphoreTake(task_manager::ui_state_mutex, pdMS_TO_TICKS(100)) == pdTRUE);
        ui.waypoint_count = 0;
        if (mx) xSemaphoreGive(task_manager::ui_state_mutex);
        return 0;
    }

    rankByDistance(center_lat, center_lon, n);

    int want = n < ui_manager::RadarConfig::MAX_WAYPOINTS ? n : ui_manager::RadarConfig::MAX_WAYPOINTS;
    selectClosest(want, n);

    struct Winner { int entry_idx; int slot; };
    static Winner winners[ui_manager::RadarConfig::MAX_WAYPOINTS];

    for (int i = 0; i < want; i++) {
        winners[i].entry_idx = g_scratch[i].idx;
        winners[i].slot = i;  // closest-first fill order; no ordering requirement downstream
    }

    std::sort(winners, winners + want, [](const Winner& a, const Winner& b) {
        const gpx_index::IndexEntry& ea = gpx_index::getEntry(a.entry_idx);
        const gpx_index::IndexEntry& eb = gpx_index::getEntry(b.entry_idx);
        if (ea.file_id != eb.file_id) return ea.file_id < eb.file_id;
        return ea.file_offset < eb.file_offset;
    });

    int materialized = 0;
    uint16_t open_file_id = 0xFFFF;
    FILE* file = nullptr;
    char filepath[256];

    for (int i = 0; i < want; i++) {
        const gpx_index::IndexEntry& e = gpx_index::getEntry(winners[i].entry_idx);

        if (!file || e.file_id != open_file_id) {
            if (file) { fclose(file); file = nullptr; }
            snprintf(filepath, sizeof(filepath), "%s/%s", GPX_FOLDER, gpx_index::getFileName(e.file_id));
            file = fopen(filepath, "rb");
            open_file_id = e.file_id;
            if (!file) {
                Serial.printf("[GPX_LOADER] ERROR: failed to reopen %s for materialization\n", filepath);
                g_slot_source[winners[i].slot] = -1;
                continue;
            }
        }

        if (fseek(file, (long)e.file_offset, SEEK_SET) != 0) {
            g_slot_source[winners[i].slot] = -1;
            continue;
        }

        int slot = winners[i].slot;
        ui_manager::Waypoint& wp = ui.waypoints[slot];

        bool mx = (task_manager::ui_state_mutex != nullptr) &&
                  (xSemaphoreTake(task_manager::ui_state_mutex, pdMS_TO_TICKS(100)) == pdTRUE);
        wp.lat = 0.0;
        wp.lon = 0.0;
        wp.valid = false;
        wp.found = false;
        wp.name[0] = '\0';
        wp.display_name[0] = '\0';
        if (wp.desc) wp.desc[0] = '\0';
        if (wp.hint) wp.hint[0] = '\0';
        if (mx) xSemaphoreGive(task_manager::ui_state_mutex);

        if (parseOneWaypointAt(file, wp)) {
            wp.found = gpx_index::getFound(winners[i].entry_idx);
            g_slot_source[slot] = winners[i].entry_idx;
            materialized++;
        } else {
            Serial.printf("[GPX_LOADER] WARNING: re-parse failed at offset %u in %s\n",
                          (unsigned)e.file_offset, filepath);
            g_slot_source[slot] = -1;
        }
    }
    if (file) fclose(file);

    bool mx2 = (task_manager::ui_state_mutex != nullptr) &&
               (xSemaphoreTake(task_manager::ui_state_mutex, pdMS_TO_TICKS(100)) == pdTRUE);
    ui.waypoint_count = want;  // some slots in [0,want) may be wp.valid==false on parse failure — same
                               // contract as the legacy loader (all readers already gate on wp.valid)
    if (mx2) xSemaphoreGive(task_manager::ui_state_mutex);

    return materialized;
}

// Reads through to "</wpt>" starting at the current file position (caller
// must fseek to the "<wpt ...>" line first — see buildFileIndex()'s
// file_offset). Mirrors parseGPXFile()'s per-waypoint state machine exactly,
// just scoped to one waypoint and committing into out_wp instead of
// ui.waypoints[waypoints_loaded].
static bool parseOneWaypointAt(FILE* file, ui_manager::Waypoint& out_wp) {
    // Working buffers (~2.4KB) live on the heap (PSRAM preferred), not on the
    // System Task's stack. Reference-to-array keeps sizeof() meaning the array.
    struct ParseBufs {
        char line[1024];
        char wpt_lat[32];
        char wpt_lon[32];
        char wp_name[48];
        char wp_display_name[64];
        char wp_desc[1024];
        char wp_hint[256];
    };
    struct BufsGuard {
        ParseBufs* p;
        BufsGuard() : p((ParseBufs*)heap_caps_calloc(1, sizeof(ParseBufs), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)) {
            if (!p) p = (ParseBufs*)heap_caps_calloc(1, sizeof(ParseBufs), MALLOC_CAP_8BIT);
        }
        ~BufsGuard() { heap_caps_free(p); }
    } guard;
    if (!guard.p) {
        Serial.println("[GPX_LOADER] ERROR: parse buffer alloc failed");
        return false;
    }
    char (&line)[1024] = guard.p->line;
    char (&wpt_lat)[32] = guard.p->wpt_lat;
    char (&wpt_lon)[32] = guard.p->wpt_lon;
    char (&wp_name)[48] = guard.p->wp_name;
    char (&wp_display_name)[64] = guard.p->wp_display_name;
    char (&wp_desc)[1024] = guard.p->wp_desc;
    char (&wp_hint)[256] = guard.p->wp_hint;
    bool in_wpt = false;
    bool in_short_desc = false;
    bool in_long_desc = false;

    LineReader lr(file);
    while (true) {
        long line_start_unused;
        bool at_eof = lr.readLine(line, sizeof(line), &line_start_unused);
        {

            if (strstr(line, "<wpt") && strstr(line, "lat=")) {
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
                    double lat = atof(wpt_lat);
                    double lon = atof(wpt_lon);
                    if (lat >= -90.0 && lat <= 90.0 && lon >= -180.0 && lon <= 180.0
                            && strlen(wpt_lat) > 0 && strlen(wpt_lon) > 0) {

                        bool mx = (task_manager::ui_state_mutex != nullptr) &&
                                  (xSemaphoreTake(task_manager::ui_state_mutex, pdMS_TO_TICKS(100)) == pdTRUE);

                        out_wp.lat = lat;
                        out_wp.lon = lon;
                        out_wp.valid = true;
                        const char* dname = strlen(wp_display_name) > 0 ? wp_display_name : wp_name;
                        int desc_len = strlen(wp_desc);
                        if (desc_len >= (int)sizeof(wp_desc) - 10) {
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
                                while (desc_len > 0 && wp_desc[desc_len-1] != ' ') desc_len--;
                                if (desc_len > 0) wp_desc[desc_len] = '\0';
                            }
                            strncat(wp_desc, " ...", sizeof(wp_desc) - strlen(wp_desc) - 1);
                        }
                        strncpy(out_wp.name,         wp_name,         sizeof(out_wp.name) - 1);
                        strncpy(out_wp.display_name, dname,           sizeof(out_wp.display_name) - 1);
                        if (out_wp.desc) strncpy(out_wp.desc, wp_desc, ui_manager::WaypointDetail::DESC_SIZE - 1);
                        if (out_wp.hint) strncpy(out_wp.hint, wp_hint, ui_manager::WaypointDetail::HINT_SIZE - 1);

                        if (mx) xSemaphoreGive(task_manager::ui_state_mutex);
                        return true;
                    }
                    return false;

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

            if (at_eof) return false;  // hit EOF before </wpt> — truncated/malformed
        }
    }
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

// Legacy single-pass loader: file/document order, capped at MAX_WAYPOINTS as
// it goes. Used only when the PSRAM index (or its scratch buffer) failed to
// allocate — see init().
static int loadAllGPXFilesLegacy() {
    Serial.println("[GPX_LOADER] Loading all GPX files from /gpx/ (legacy, no index)...");

    DIR* dir = opendir(GPX_FOLDER);
    if (!dir) {
        Serial.println("[GPX_LOADER] ERROR: Failed to open gpx folder");
        return 0;
    }

    files_loaded = 0;
    waypoints_loaded = 0;
    was_truncated = false;
    working_set_capped = false;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        const char* fname = entry->d_name;
        size_t nl = strlen(fname);
        if (nl >= 4 && strcasecmp(fname + nl - 4, ".gpx") == 0) {
            char filepath[256];
            snprintf(filepath, sizeof(filepath), "%s/%s", GPX_FOLDER, fname);

            int parsed = parseGPXFile(filepath);
            if (parsed > 0) {
                files_loaded++;
            } else {
                Serial.printf("[GPX_LOADER] WARNING: %s yielded 0 waypoints\n", fname);
            }
        }
    }
    closedir(dir);

    Serial.printf("[GPX_LOADER] Loading complete: %d files, %d waypoints%s\n",
                  files_loaded, waypoints_loaded, was_truncated ? " (truncated)" : "");

    return waypoints_loaded;
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

    // Geocaching.com GPX files end with </wpt></gpx> with NO trailing newline.
    // Treat EOF as a final newline so the last buffered chunk is always processed.
    LineReader lr(file);
    while (waypoints_loaded < ui_manager::RadarConfig::MAX_WAYPOINTS) {
        long line_start_unused;
        bool at_eof = lr.readLine(line, sizeof(line), &line_start_unused);
        {

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
                        // desc/hint are pointers into a PSRAM block (see ui_manager::init());
                        // null only if that allocation failed.
                        if (wp.desc) strncpy(wp.desc, wp_desc, ui_manager::WaypointDetail::DESC_SIZE - 1);
                        if (wp.hint) strncpy(wp.hint, wp_hint, ui_manager::WaypointDetail::HINT_SIZE - 1);
                        waypoints_loaded++;
                        ui.waypoint_count = waypoints_loaded;

                        if (mx) xSemaphoreGive(task_manager::ui_state_mutex);

                        waypoints_parsed++;
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

            if (at_eof) break;  // Exit after flushing final line (no trailing newline in geocaching GPX)
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
        ui.waypoints[i].found = false;
        g_slot_source[i] = -1;
    }

    // Reset module-static counters inside the same mutex-held region as the
    // array clear (previously done just outside it — a stale-read window).
    waypoints_loaded = 0;
    files_loaded = 0;
    was_truncated = false;
    working_set_capped = false;

    if (mutex_acquired) {
        xSemaphoreGive(task_manager::ui_state_mutex);
    }

    if (g_index_available) {
        gpx_index::reset();
    }
}

void getStats(int& file_count, int& waypoint_count, bool& truncated) {
    file_count = files_loaded;
    waypoint_count = waypoints_loaded;
    truncated = was_truncated;
}

void getIndexStats(int& index_count, bool& index_truncated, bool& working_set_capped_out) {
    index_count = g_index_available ? gpx_index::getEntryCount() : waypoints_loaded;
    index_truncated = was_truncated;
    working_set_capped_out = working_set_capped;
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
