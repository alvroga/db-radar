# ADR-0001: Cache waypoint desc/hint in PSRAM rather than re-reading the GPX file on tap

Status: Accepted
Date: 2026-07-31
Decided by: Claude (proposed, you approved)

## Context

`g_ui_state` was the largest symbol in the firmware (70,992 B, ~37% of static RAM), almost entirely
`Waypoint::desc[1024]` + `Waypoint::hint[256]` × `MAX_WAYPOINTS` (50) — read in exactly one place
(`waypoint_screen.cpp`, one waypoint at a time when its detail screen is opened) but resident in SRAM
for all 50 permanently. Two ways to stop paying for that: keep the data somewhere cheaper (PSRAM), or
stop keeping it at all and re-read the source GPX file when a waypoint is tapped.

Re-reading was the more resource-frugal option on paper, but `parseGPXFile()` doesn't retain the
originating file path per waypoint today — `loadAllGPXFiles()` builds `filepath` on the stack and
discards it once parsing returns. Re-reading on tap would need a `source_file`/offset field added to
`Waypoint`, plus a second parse path that seeks to one waypoint's `<desc>`/`<hint>` tags instead of
committing a whole record, and a user-visible stall (SD file open + partial parse) on every tap.

## Decision

Cache `desc`/`hint` for all 50 waypoints in PSRAM instead. Added a `WaypointDetail` struct holding
just those two fields, allocated as one block via `heap_caps_calloc(MAX_WAYPOINTS,
sizeof(WaypointDetail), MALLOC_CAP_SPIRAM)` in `ui_manager::init()`. `Waypoint::desc`/`hint` became
`char*` pointers into that block instead of embedded arrays — the one write site
(`gpx_loader.cpp`) and one read site (`waypoint_screen.cpp`) are otherwise unchanged.

No section attributes (`.ext_ram_noinit` boot-crashes on this IDF — constructors aren't run for
objects placed there, per the existing ROADMAP warning). Allocation failure is handled by leaving the
pointers `nullptr` and guarding both call sites, not by crashing or falling back to SRAM.

## Consequences

**Easier**: zero behavior change at the two call sites beyond a null check: the tap-to-view latency,
GPX parsing code, and buffer-truncation logic are all untouched. PSRAM is 8 MB and effectively free at
this project's usage level, so 64 KB of permanent residency there costs nothing that matters.

**Harder**: `sizeof(wp.desc)` is no longer `1024` (it's `sizeof(char*)` = 8) — call sites must use
`WaypointDetail::DESC_SIZE`/`HINT_SIZE` explicitly. This class of bug (a `sizeof` silently changing
meaning after a field becomes a pointer) is exactly the kind the project's other tuned-constant
mistakes have come from; it was caught in review before shipping, not by a test.

**Gave up**: this does not by itself raise `MAX_WAYPOINTS` past 50 — it only frees the SRAM that a
higher cap would need. Re-reading from GPX would have scaled further (no PSRAM ceiling for waypoint
count), but at 50→~500 waypoints PSRAM headroom this isn't the binding constraint yet; if it ever is,
this decision should be revisited rather than assumed permanent.
