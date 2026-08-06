# Plan: Two-Tier Waypoint Index (PSRAM full index + SRAM working set)

**Status**: Implemented 2026-08-05 (all four phasing steps, §9). Build-clean, SRAM cost measured
(+3,640 B static, `MAX_WAYPOINTS` held at 200 throughout), and **field-verified on real hardware**
against an independent Haversine oracle (cold selection, a forced high-churn reselect, HDOP gating, a
live end-to-end nearby-waypoints test) — see [ADR-0023](docs/adr/0023-two-tier-waypoint-index.md) for
the full record, the two real bugs this verification pass caught and fixed, and what §10 items (below)
are still open (automatic-trigger observation from real movement; concurrent SD access safety).
Originally written 2026-08-05 as a design-only document.

## Context

`RadarConfig::MAX_WAYPOINTS` (`include/ui/ui_manager.h:72`) is 200 — a hard SRAM ceiling, not a
render-cost ceiling (see ADR-0022 and its addendum: raising it to 500 broke boot because
`ui.waypoints[MAX_WAYPOINTS]` is a fixed-size array living in static SRAM, and free heap at boot,
not the static-RAM percentage, is the real constraint).

Today `gpx_loader::loadAllGPXFiles()` (`src/gpx/gpx_loader.cpp:38-80`) iterates GPX files via
`readdir()` (filesystem order) and fills `ui.waypoints[]` in file/document order until the 200 cap
is hit — completely independent of distance to the user. A user with 1000+ waypoints across many
files (e.g. 20 "quest" GPX files with 20 waypoints each, or a large pocket query) gets whichever 200
happened to be encountered first, which could all be on the other side of the world.

**Goal**: store every waypoint (thousands, in PSRAM — abundant, not the scarce resource) in a
lightweight index, and keep the 200-slot SRAM array as a "live working set" populated with the N
closest to the user's actual position, refreshed on load and as the user moves.

## Design

### 1. New PSRAM index — `include/gpx/gpx_index.h` / `src/gpx/gpx_index.cpp` (new module)

```cpp
namespace gpx_index {
constexpr int MAX_INDEX_ENTRIES = 8192;   // ~128-156KB PSRAM — thousands, not millions
constexpr int MAX_INDEX_FILES   = 64;

struct IndexEntry {
    float    lat = 0.0f;         // float OK — sort/distance key only, pass 2 re-parses full double
    float    lon = 0.0f;
    uint32_t file_offset = 0;    // ftell() at the start of the "<wpt" line — new, no ftell() exists today
    uint8_t  file_id = 0;        // -> IndexFile table
    bool     found = false;      // source of truth for "found" across working-set churn, see §6
};
struct IndexFile { char name[96] = {}; };

bool init();  // heap_caps_calloc both arrays in PSRAM, called from ui_manager::init() alongside WaypointDetail
int  getEntryCount();
bool wasIndexTruncated();   // true only if total waypoints > MAX_INDEX_ENTRIES (rare)
}
```

Follow the exact `WaypointDetail` PSRAM pattern (`src/ui/ui_manager.cpp:38-52`):
`heap_caps_calloc(count, sizeof, MALLOC_CAP_SPIRAM)`, log-and-degrade on failure (never crash — if
the index alloc fails, fall back to today's single-pass file-order behavior).

### 2. `gpx_loader.cpp` two-pass split

- **Pass 1 (cheap, uncapped)**: new `buildFullIndex()` — walks every GPX file, scans only for
  `<wpt lat=... lon=...>` lines (reusing `extractAttribute()`, `gpx_loader.cpp:430-453`), records
  `ftell()` at line start + lat/lon into the PSRAM index. No field parsing (name/desc/hint), no
  `MAX_WAYPOINTS` cap.
- **Selection**: `selectAndMaterialize(center_lat, center_lon)` — Haversine-distance every index
  entry against the user's position, `std::partial_sort` to the closest `MAX_WAYPOINTS` (§3).
- **Pass 2 (capped)**: `parseOneWaypointAt(FILE*, Waypoint&)` — extracted from today's
  per-waypoint state machine (`gpx_loader.cpp:178-367`, the `<name>`/groundspeak/`<desc>`/`<cmt>`
  dispatch), unchanged in behavior, just scoped to one waypoint and taking an out-param instead of
  writing directly into `ui.waypoints[waypoints_loaded]`. Seeks to each winning entry's
  `file_offset` and materializes it into a working-set slot.
- `loadAllGPXFiles()` becomes: `clearWaypoints()` → `buildFullIndex()` →
  `selectAndMaterialize(ui.center_lat, ui.center_lon)`.
- `clearWaypoints()` also clears the index; fix the pre-existing bug where it resets its file-static
  counters just outside the mutex-held region (`gpx_loader.cpp:414-418`) while touched.
- `was_truncated` keeps its meaning (index itself exceeded `MAX_INDEX_ENTRIES` — rare, real loss);
  add a new `working_set_capped` (index > `MAX_WAYPOINTS` — expected/normal now, not a warning).

### 3. Closest-N selection

`std::partial_sort` over a PSRAM scratch array of `{distance, entry_idx}` (allocated once,
`MAX_INDEX_ENTRIES` entries) — O(n log N). At n≤8192, N=200 this is ~62,000 comparisons, trivial at
240MHz and infrequent (load/refresh/upload/delete + movement trigger, not per-frame).

### 4. Haversine helper — `include/utils/geo.h` / `src/utils/geo.cpp` (new)

```cpp
namespace geo {
constexpr double EARTH_RADIUS_M = 6371000.0;
double haversineMeters(double lat1, double lon1, double lat2, double lon2);
}
```

Used by the index sort and the movement trigger. Deliberately **not** the equirectangular
approximation `navigation.cpp` uses for rendering (ADR-0022) — that's only accurate at radar scale
(≤ a few km, per the comment at `navigation.cpp:261-264`); candidate waypoints here can be globally
distributed. `navigation.cpp`'s existing four inline distance call sites are left untouched — out of
scope, avoids churn in a working, already-ADR'd path.

### 5. Movement-triggered reselection

Location: `src/utils/task_manager.cpp`, `updateStatusLabels()`, next to the WMM declination block
(~line 1566) and the `RADAR_REFRESH` queue push (~line 1638) — same task (System Task, 10Hz), same
`gps_position_valid && gps_data.valid` gate.

Use `geo::haversineMeters()` directly for the trigger check, **not** the WMM's isotropic
degrees²-sum shortcut — that shortcut is tuned for a ~100km threshold where ignoring `cos(lat)` is a
small error; at a tight ~150-200m reselect threshold the same shortcut is off by >2x at mid/high
latitudes for east-west movement. The Haversine call itself is cheap (one call/tick); only the
reselect body is expensive, so correctness costs nothing here.

Only runs when `gpx_index::getEntryCount() > MAX_WAYPOINTS` (otherwise everything indexed is already
loaded). Does **delta materialization**: diff the new closest-200 set against a PSRAM
`int16_t g_slot_source[MAX_WAYPOINTS]` (slot → index-entry id, built at the last select), and only
`fseek`+reparse entries that actually changed (typically 0-5 per trigger) — avoids repeated SD I/O
every tick. No new queueing needed: it only mutates `ui.waypoints[]`, and the existing GPS branch
already queues `RADAR_REFRESH` every valid tick regardless.

### 6. Working-set stability (new correctness concern this design introduces)

- **`fixed_waypoint_index`/`selected_waypoint_index`** (`ui_manager.h:151-152`) are currently
  long-lived slot numbers, safe because `ui.waypoints[]` was previously only rewritten by explicit
  user action. Once System Task can silently swap slot contents every ~200m of movement, a stale
  index can point at the wrong waypoint (wrong-target sonar, wrong detail screen). Fix: before any
  reselect, if `fixed_waypoint_index >= 0`, capture its `(lat, lon)`; after, re-resolve by exact
  match or clear to -1 (already handled safely everywhere as "no fix"). Clear
  `selected_waypoint_index` unconditionally on any reselect. Consider skipping the *periodic* (not
  full-rebuild) reselect while the waypoint detail screen is active.
- **`Waypoint::found`** must survive working-set churn: make `IndexEntry.found` the source of truth,
  write-through from `handleTapAt()`'s found-marking (`navigation.cpp:1413-1416`) via
  `gpx_index::markFound()`, and seed it back on every materialization.
- **Concurrency audit**: System Task becomes an ongoing second writer to `ui.waypoints[]` for the
  app's lifetime, not just at load events. Audit every UI-Task-only direct write
  (`handleTapAt()`'s `found`/`selected_waypoint_index` writes at `navigation.cpp:1401,1414`,
  `waypoint_screen.cpp`'s fix/unfix at lines 214-220) for missing `ui_state_mutex` coverage — these
  were implicitly safe before and won't be once reselect is live.

### 7. Trigger points

| Site | Change |
|---|---|
| `main.cpp:284` (boot) | `loadAllGPXFiles()` unchanged signature, two-pass internally. Depends on §8. |
| `settings_screen.cpp:2042` (refresh button) | unchanged call site. |
| `gpx_server.cpp:1008` (`upload_handler`) | unchanged, still queues `RADAR_REFRESH`. |
| `gpx_server.cpp:1120` (`delete_handler`) — **pre-existing gap** | currently never triggers a reload at all (`remove()` only). Close it: call `refreshGPXFiles()` + queue `RADAR_REFRESH`, matching upload. Becomes a real correctness requirement once the index can hold stale `file_offset`s into a deleted file. |

### 8. Wire NVS last-known position into `ui.center_lat`/`center_lon` at boot

`settings_manager::loadGPSState()` (`src/utils/settings_manager.cpp:666-691`) is currently read only
at `device_manager.cpp:316` to pick GPS hot/warm/cold start timing — its lat/lon is **never** written
into `ui.center_lat/center_lon`. Without this, the very first boot-time selection (before any live
fix) sorts against `(0,0)` or the hardcoded default (`ui_manager.cpp:395-396`), which isn't
meaningfully better than today's arbitrary truncation. Thread the loaded position into
`ui.center_lat/center_lon` at the same call site, before `loadAllGPXFiles()` runs
(`main.cpp:284`). Superseded naturally once a live fix lands (`navigation.cpp:1240-1243` already
overwrites it).

### 9. Phasing

1. `geo.h`/`geo.cpp` + `gpx_index` struct/alloc — no behavior change, independently testable.
2. §8's NVS boot-position wiring — small, valuable on its own.
3. `gpx_loader.cpp` two-pass split + selection, wired into the existing (single) load path — boot/
   refresh/upload/delete triggers all get correct closest-N behavior. **Shippable checkpoint.**
4. Movement-triggered reselect (§5) + working-set stability (§6) + the concurrency audit. More
   delicate (new concurrency surface), deserves its own review/test cycle.

### 10. Verification

Status as of the 2026-08-05 hardware pass (see ADR-0023 for full detail):

- ✅ **Done.** Uploaded `assets/gpx/TEST_GLOBAL_500.GPX` alongside real `/sdcard/gpx/` files (16 files,
  515 waypoints total); confirmed working set = 200 nearest to actual position, cross-checked against
  an independent oracle script for two different centers (including a forced high-churn case), not
  first-200-in-file-order.
- ⚠️ **Partially done, and the premise was wrong.** Measured the static SRAM delta via `readelf -S`
  `.dram0.bss`/`.dram0.data` diff: **+3,640 B**, not the "~0" this line originally predicted — small,
  but a real, nonzero cost (the selection scratch/delta-diff bookkeeping arrays). The free-internal-
  heap-before-BLE-init check specifically (the metric ADR-0022's rollback was actually verified
  against, distinct from the static-SRAM percentage) was **not** repeated for this feature — still
  open if BLE-active heap margin at boot needs re-confirming.
- ✅ **Done**, except "last reselect distance/time" was not added. `gpx index` reports index count,
  working-set size, truncation flags, PSRAM bytes used, and center position. `gpx index list`/
  `reselect`/`gentest` were also added as debug-only verification tools (not in this line's original
  scope, but ended up doing the same job).
- ⚠️ **Partially done.** Did not time `buildFullIndex()` or the first cold `selectAndMaterialize()`
  separately, but observed a full-churn `reselect()` (174/200 slots re-parsed from real SD) take
  457ms — comfortably under the 30s TWDT budget, but a useful worst-case data point.
- ⚠️ **Partially done.** Confirmed delta reselect fires correctly and doesn't stall (37–457ms observed,
  via synthetic centers injected over serial, not a real GPS-driven trigger) and doesn't corrupt state.
  **Not done**: an actual field walk with live GPS driving the automatic trigger; `found`/`fixed`
  surviving a real walk-away-and-back specifically through `fixed_waypoint_index` (needs a touchscreen
  interaction this pass didn't drive).
- ❌ **Still fully open.** No SD-access mutex exists anywhere in the codebase; concurrent access
  (trigger an upload mid-reselect) was not stress-tested. FATFS/VFS is assumed, not proven, to
  serialize this safely.

### 11. ADR

Warrants one (`docs/adr/0023-...`) — a real choice between alternatives: (a) raise `MAX_WAYPOINTS`
further (rejected — the exact SRAM mistake ADR-0022 already made once), (b) per-file caps in file
order (rejected — still geography-blind), (c) this two-tier design (chosen — spends the abundant
resource, PSRAM, to solve the actual problem while keeping SRAM fixed). Should explicitly close the
"Still open" item ADR-0022/ROADMAP left dangling.

### Critical files

- `src/gpx/gpx_loader.cpp`, `include/gpx/gpx_loader.h` — two-pass split
- `include/gpx/gpx_index.h`, `src/gpx/gpx_index.cpp` — new module
- `include/utils/geo.h`, `src/utils/geo.cpp` — new Haversine helper
- `include/ui/ui_manager.h`, `src/ui/ui_manager.cpp` — PSRAM alloc call site, stats
- `src/utils/task_manager.cpp` — movement-triggered reselect
- `src/gpx/gpx_server.cpp` — delete-handler trigger gap
- `src/ui/navigation.cpp`, `src/ui/waypoint_screen.cpp` — working-set stability, mutex audit
- `src/core/device_manager.cpp` — NVS boot-position wiring

### Test fixture

`assets/gpx/TEST_GLOBAL_500.GPX` — 500 synthetic waypoints scattered across ~15 world regions,
already generated and in the repo (local copy only, not yet on-device).
