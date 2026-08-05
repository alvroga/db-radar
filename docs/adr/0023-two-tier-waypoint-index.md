# ADR-0023: Two-tier waypoint index (PSRAM full index + SRAM working set)

Status: Accepted
Date: 2026-08-05
Decided by: Claude (proposed), you approved via "start implementing"

## Context

`RadarConfig::MAX_WAYPOINTS` (`include/ui/ui_manager.h`) is 200 — a hard SRAM ceiling, not a
render-cost ceiling. ADR-0022 raised it 50→500 for headroom, then rolled back to 200 the same day
after 500 failed to boot (`xTaskCreatePinnedToCore` returned `pdFAIL` — free heap at boot, not the
static-RAM percentage, was the real constraint). That ADR's Consequences section explicitly left the
next step open: "the next step is not 'raise the constant again' casually — it's re-running this same
SRAM-budget table," and ROADMAP.md's "Still open" note named exactly two options, both spending more
SRAM (accept a thinner BLE-active margin, or move `Waypoint waypoints[MAX_WAYPOINTS]` into PSRAM
outright, which the same note flagged as risky given `drawWaypoints()` reads lat/lon/valid/found for
every *loaded* waypoint every frame).

Separately, `gpx_loader::loadAllGPXFiles()` fills `ui.waypoints[]` in filesystem/document order via
`readdir()` — completely independent of distance to the user. A user with 1,000+ waypoints across many
files (e.g. several "quest" GPX files, or a large pocket query) gets whichever 200 happened to be
encountered first, which could be on the other side of the world from their actual position. Raising
the cap doesn't fix this — it only delays which waypoint count triggers it.

## Decision

Keep `MAX_WAYPOINTS = 200` (the SRAM ceiling stays exactly where ADR-0022 field-verified it boots) and
add a second, much larger index in PSRAM — abundant, not the scarce resource here — that records every
waypoint across every GPX file as a lightweight `{lat, lon, file_offset, file_id, found}` record
(`gpx_index`, `MAX_INDEX_ENTRIES = 8192`, ~156KB PSRAM). The 200-slot SRAM array becomes a working set:
closest-N to the user's actual position, computed via Haversine + `std::partial_sort` over the PSRAM
index, and refreshed both at load time and as the user moves (>150m, checked at System Task's 10Hz
tick, only when the index actually holds more than the working set).

Three real alternatives were on the table:

- **(a) Raise `MAX_WAYPOINTS` further.** Rejected — this is the exact mistake ADR-0022 already made
  once; there is no cap that isn't eventually exceeded by a large enough pocket query, and every step
  up costs static SRAM against the same 80%-utilization budget line with no compensating benefit for
  users below the old cap.
- **(b) Per-file caps in file order** (e.g. first 40 waypoints per file, up to 200 total). Rejected —
  still geography-blind; a single dense local file would starve out every other file's waypoints, and
  a single sparse global file would still crowd out nearby ones from a different file.
- **(c) This two-tier design.** Chosen — spends PSRAM, which this project already treats as abundant
  (`WaypointDetail`, ADR-0001), to solve the actual problem (waypoint *selection*, not waypoint
  *storage*) while leaving the SRAM footprint exactly where ADR-0022's boot failure said it must stay.

This closes ADR-0022/ROADMAP.md's "still open" item on its own terms: getting useful behavior past 200
waypoints no longer requires spending more SRAM at all.

## Consequences

**Easier**: a user with any number of waypoints across any number of files sees the 200 closest to
their actual position, not an arbitrary filesystem-order slice. A GPX upload or delete now correctly
triggers reselection (the delete path previously never reloaded at all — closed as part of this
change, `gpx_server.cpp`'s `delete_handler`). The working set updates as the user walks, without a
full rescan: `gpx_loader::reselect()` diffs the new closest-200 against the previous slot mapping and
only re-parses (`fseek` + re-run the per-waypoint state machine) the 0–a few entries that actually
changed, not the whole set.

**Harder**: `ui.waypoints[]` is no longer written only at discrete load events by whichever task calls
`loadAllGPXFiles()`. The System Task's movement-triggered `reselect()` is now a second, ongoing writer
for the app's lifetime. `fixed_waypoint_index`/`selected_waypoint_index` needed new handling because a
slot's contents can now change out from under a long-lived index: `reselect()` keeps a surviving
entry's slot number stable (it never moves once assigned), so `fixed_waypoint_index` survives a
reselect by comparing its slot's source-entry id before/after — if unchanged, still valid; if the slot
got recycled for a different entry, cleared to -1. `selected_waypoint_index` is cleared defensively on
every reselect, and the reselect itself is skipped entirely while the waypoint detail screen
(`ui.screen_waypoint`) is open, so a slot can't be recycled out from under a screen the user is
actively looking at. `Waypoint::found` now has `IndexEntry.found` (in the PSRAM index, keyed by entry,
not slot) as its source of truth, written through from `handleTapAt()`'s found-marking via
`gpx_loader::markWaypointFound()`, so it survives the slot being reused by a later reselect. Three
UI-Task write sites (`navigation.cpp`'s tap handler, `waypoint_screen.cpp`'s fix/unfix button) picked
up `ui_state_mutex` protection for the struct-valued writes they make, matching the convention
CLAUDE.md already documents for cross-task shared state — the plain-`int` index fields themselves
don't need it (single-word writes are already atomic on Xtensa), but the accompanying `Waypoint`
struct fields do.

**Gave up / left open**: the render-path *reads* of `ui.waypoints[]` (`drawWaypoints()`,
`updateWaypointFixSonar()`, hit-testing) remain unprotected by `ui_state_mutex`, matching this
codebase's pre-existing convention (writers take the mutex, most readers don't) rather than a full
reader-side audit — that would add mutex contention to the render hot path this project has
specifically optimized to avoid stalling (see the Render Pipeline section of CLAUDE.md). The
consequence is a narrow, rare torn-read window (a render frame or a `markWaypointFound()` call landing
mid-`reselect()`) whose worst case is a cosmetic glitch on one frame or one mislabeled found-state, not
a crash — judged acceptable at the reselect's ~150m/movement trigger rate. Also: no SD-access mutex
exists anywhere in this codebase, and `reselect()` adds a new concurrent SD reader (periodic, on the
System Task) alongside the existing ones (upload, `field_log`'s writer, boot load) — this was flagged
as a verification item in the design doc, not resolved by it; FATFS/VFS is assumed, not proven, to
serialize concurrent access safely under this project's setup.

**Measured cost**: static SRAM (`readelf -S` `.dram0.data`+`.dram0.bss` diff, `MAX_WAYPOINTS` held at
200 throughout so this isolates the feature from ADR-0022's unrelated cap change) — 154,656 B baseline
→ 158,296 B with the full feature (movement reselect included): **+3,640 B (+1.1 percentage points)**.
PSRAM: `gpx_index` (8192 × ~14B entries + 64 × 96B file names ≈ 121.5KB) + the selection scratch buffer
(8192 × 16B ≈ 128KB) ≈ 250KB — well under 16% of the 8MB chip even before counting `WaypointDetail`.

## Verification status

Build-clean on ESP-IDF (`pio run -e cc-radar`), SRAM delta measured as above. **Not yet field-tested on
hardware** — no on-device confirmation yet that: a real multi-file/high-count GPX load selects the
correct closest-200; the movement-triggered reselect fires at the right threshold without stalling the
System Task loop or tripping the TWDT; `found`/`fixed` survive a real walk-away-and-back; concurrent SD
access (upload mid-reselect) doesn't corrupt state. Treat this ADR as documenting the design decision
and initial implementation, not a closed field-verification loop — see
`docs/waypoint_two_tier_index_plan.md` §10 for the full verification checklist this still owes.
