#include "gpx/gpx_index.h"
#include "core/arduino_compat.h"
#include "esp_heap_caps.h"
#include <string.h>

namespace gpx_index {

struct IndexFile {
    char name[96] = {};
};

static IndexEntry* g_entries = nullptr;
static IndexFile*  g_files = nullptr;

static int  g_entry_count = 0;
static int  g_file_count = 0;
static bool g_truncated = false;
static bool g_alloc_ok = false;

bool init() {
    g_entries = (IndexEntry*)heap_caps_calloc(MAX_INDEX_ENTRIES, sizeof(IndexEntry), MALLOC_CAP_SPIRAM);
    g_files   = (IndexFile*)heap_caps_calloc(MAX_INDEX_FILES, sizeof(IndexFile), MALLOC_CAP_SPIRAM);

    if (!g_entries || !g_files) {
        Serial.println("[GPX_INDEX] ERROR: PSRAM alloc failed — full index disabled");
        g_alloc_ok = false;
        return false;
    }

    g_alloc_ok = true;
    Serial.printf("[GPX_INDEX] PSRAM index ready: %d entries x %u bytes, %d files x %u bytes\n",
                  MAX_INDEX_ENTRIES, (unsigned)sizeof(IndexEntry),
                  MAX_INDEX_FILES, (unsigned)sizeof(IndexFile));
    return true;
}

void reset() {
    g_entry_count = 0;
    g_file_count = 0;
    g_truncated = false;
    // Entries/files themselves are overwritten as new ones are added; no need
    // to zero MAX_INDEX_ENTRIES worth of PSRAM on every reload.
}

int addFile(const char* name) {
    if (!g_alloc_ok) return -1;
    if (g_file_count >= MAX_INDEX_FILES) return -1;
    int id = g_file_count++;
    strncpy(g_files[id].name, name, sizeof(g_files[id].name) - 1);
    return id;
}

bool addEntry(float lat, float lon, uint32_t file_offset, uint16_t file_id) {
    if (!g_alloc_ok) return false;
    if (g_entry_count >= MAX_INDEX_ENTRIES) {
        g_truncated = true;
        return false;
    }
    IndexEntry& e = g_entries[g_entry_count];
    e.lat = lat;
    e.lon = lon;
    e.file_offset = file_offset;
    e.file_id = file_id;
    e.found = false;
    g_entry_count++;
    return true;
}

int getEntryCount() {
    return g_entry_count;
}

bool wasIndexTruncated() {
    return g_truncated;
}

const IndexEntry& getEntry(int idx) {
    return g_entries[idx];
}

const char* getFileName(uint16_t file_id) {
    if (!g_alloc_ok || file_id >= g_file_count) return "";
    return g_files[file_id].name;
}

void setFound(int idx, bool found) {
    if (!g_alloc_ok || idx < 0 || idx >= g_entry_count) return;
    g_entries[idx].found = found;
}

bool getFound(int idx) {
    if (!g_alloc_ok || idx < 0 || idx >= g_entry_count) return false;
    return g_entries[idx].found;
}

size_t getPsramBytesUsed() {
    if (!g_alloc_ok) return 0;
    return (size_t)MAX_INDEX_ENTRIES * sizeof(IndexEntry) + (size_t)MAX_INDEX_FILES * sizeof(IndexFile);
}

} // namespace gpx_index
