#pragma once
#include <stdint.h>
#include <stddef.h>

namespace gpx_index {

constexpr int MAX_INDEX_ENTRIES = 8192;   // ~128-156KB PSRAM — thousands, not millions
// 512 -> 1024 2026-08-07: 512 loaded fast in the field test (docs/adr/0028), so raised the
// ceiling for one more stress-test data point. IndexFile is 96B/slot -- +49,152B PSRAM
// (512->1024), trivial. Not a design requirement: docs/quests_plan.md's badge worst-case
// math is against <=512. Field-verified working at 1024 (2026-08-07, see CHANGELOG.md) --
// the boot-time slowdown observed in that test was the loading-spinner-freeze bug (also
// fixed 2026-08-07), not a real cost of the higher cap.
constexpr int MAX_INDEX_FILES   = 1024;
static_assert(MAX_INDEX_FILES < 0xFFFF, "0xFFFF is gpx_loader.cpp's open_file_id sentinel");

// Lightweight per-waypoint record for the full (uncapped) PSRAM index.
// lat/lon are float — this is a sort/distance key only; pass 2 re-parses the
// full double precision value from the source file at materialization time.
struct IndexEntry {
    float    lat = 0.0f;
    float    lon = 0.0f;
    uint32_t file_offset = 0;    // ftell() at the start of the "<wpt" line
    uint16_t file_id = 0;        // -> file table, see getFileName(). Widened from
                                  // uint8_t 2026-08-07 — free, absorbed by existing
                                  // struct padding (see docs/quests_plan.md §0.5).
    bool     found = false;      // source of truth for "found" across working-set churn
};

// heap_caps_calloc both backing arrays in PSRAM. Log-and-degrade on failure —
// never crash; callers must treat a false return as "index unavailable" and
// fall back to whatever behavior doesn't need it.
bool init();

// Drop all entries/files (does not free the PSRAM allocation). Call before
// re-scanning (refresh/upload/delete).
void reset();

// Register a source file, returns its file_id, or -1 if MAX_INDEX_FILES is exhausted.
int addFile(const char* name);

// Append one entry. Returns false (and sets the truncated flag) once
// MAX_INDEX_ENTRIES is reached — rare, real loss, distinct from the working
// set being smaller than the index (expected/normal).
bool addEntry(float lat, float lon, uint32_t file_offset, uint16_t file_id);

int  getEntryCount();
bool wasIndexTruncated();   // true only if total waypoints > MAX_INDEX_ENTRIES

// idx must be < getEntryCount(); no bounds checking (hot path, called from
// partial_sort comparators).
const IndexEntry& getEntry(int idx);
const char* getFileName(uint16_t file_id);

// Write-through found-state, keyed by index entry (not working-set slot —
// slots get recycled by reselect, entries don't).
void setFound(int idx, bool found);
bool getFound(int idx);

size_t getPsramBytesUsed();

} // namespace gpx_index
