# ADR-0022: Raise MAX_WAYPOINTS to 500 (not 700+), after replacing Haversine with equirectangular

Status: Superseded on the cap number — see addendum below. Render-cost reasoning (Haversine →
equirectangular) is unaffected.
Date: 2026-08-05
Decided by: You (after Claude's analysis)

## Addendum (2026-08-05, same day): 500 doesn't boot

The static-SRAM budget table below was checked against the documented 80% caution line and passed
(60.4% baseline / 68.2% BLE-active). It was never checked against actual free heap at boot, which is
a different — and tighter — resource: `xTaskCreatePinnedToCore` grabs task stacks from live internal
heap, not from the static budget. On the first real boot at cap=500, `[BEACON] Free internal SRAM
before BLE init: 83899 bytes` — BLE (~25KB) plus the UI (16KB) and I2C (8KB) task stacks it hands out
first left too little for the Network (12KB) and System (8KB) tasks after; both `xTaskCreate` calls
returned `pdFAIL` and the device never came up. This is exactly the gap the "Field verification still
open" section below named and didn't close before shipping.

`MAX_WAYPOINTS` is now **200** (`include/ui/ui_manager.h`), not 500 — see the table below, whose own
row for 200 (154,656 B / 47.2% baseline, 55.0% BLE-active) is what got picked, on the reasoning that a
number the table had already computed was safer than guessing a new one under time pressure.
**Field-confirmed booting on hardware 2026-08-05, BLE active.** That boot only had 15 real waypoints
loaded — the array is fixed-size regardless of fill count, so the confirmation validates the 200-slot
SRAM footprint, not a 200-waypoint GPX load. 200 is therefore a conservative floor, not a measured
ceiling: the true boundary sits somewhere in the untested 200–500 range, and the table's own 300 row
(51.6%/59.4%) is the next candidate if more headroom is wanted — but it would need the same kind of
hardware boot verification before being trusted, not just a clean `pio run` size check, which is what
let 500 through the first time.

The options for getting back to 500+ are noted in ROADMAP.md's Waypoint Memory Optimization entry:
either accept a much thinner BLE-active heap margin than this project's other budget decisions assume,
or move `Waypoint waypoints[MAX_WAYPOINTS]` into PSRAM the way `WaypointDetail` already is — a bigger
change, since lat/lon and the found/valid flags are read every frame for every *loaded* waypoint (not
just visible ones) in `drawWaypoints()`'s filter pass. Neither has been evaluated yet.

## Context

`RadarConfig::MAX_WAYPOINTS = 50` was a load-time cap dating from before the PSRAM migration
(ADR-0001). `gpx_loader` silently truncates any GPX file past 50 waypoints — a real geocaching.com
pocket query routinely has hundreds. `docs/waypoint_cap_increase_investigation.md` (2026-08-04) did
the full analysis before any code changed: cost is +144 B SRAM / +1,280 B PSRAM per waypoint
(`sizeof(Waypoint)` measured directly via cross-compile, not assumed), and the per-waypoint cost that
actually scales with the cap is `drawWaypoints()`'s Haversine loop — 10 double-precision transcendental
calls per waypoint, unconditional, running on an FPU that's single-precision only.

Two decisions were bundled here, in sequence:

**1. How to cut the per-waypoint render cost.** The investigation's proposed fix — an equirectangular
approximation (accurate to sub-pixel at radar scale) in place of Haversine — was implemented first,
in `drawWaypoints()` and `latLonToScreen()` (`src/ui/navigation.cpp`), as its own isolated change:
10 double transcendental calls/waypoint became 2 multiplies + 1 `sqrtf`, plus `atan2f` only for
waypoints that end up off-screen (bearing is otherwise unused). Two more `cos`/`sin` calls
(`cos_lat2`/`sin_lat2`), no longer needed once distance/bearing aren't derived via Haversine, were
also dropped — a saving the investigation doc hadn't anticipated. Field-verified on hardware
(2026-08-05) at the current real-world waypoint count: no regression, "works perfectly."

**2. What number to raise the cap to.** This is a real product tradeoff, not just an engineering one,
because the SRAM this spends isn't earmarked for anything — an audit of every ROADMAP.md "Planned"/
"Known Issues" entry found no feature with a competing claim on it. That makes the ceiling a judgment
call about how much of the project's own 80%-utilization caution line to spend now versus reserve.
Measured against the current build (133,056 B / 40.6% static SRAM baseline, cap=50):

| Cap | Static SRAM | % (BLE idle) | % (BLE active, 50m zoom) |
|---|---|---|---|
| 200 | 154,656 B | 47.2% | 55.0% |
| 300 | 169,056 B | 51.6% | 59.4% |
| **500** | 197,856 B | 60.4% | **68.2%** |
| 700 | 226,656 B | 69.2% | 77.0% |
| 750 | 233,856 B | 71.4% | 79.2% ← edge of the caution line |
| 1000 | 269,856 B | 82.4% | 90.2% ← over it |

PSRAM was never the binding constraint at any of these (even 1,000 waypoints is <16% of the 8 MB
chip). 500 also isn't an arbitrary round number: it's geocaching.com's standard Basic-membership
Pocket Query size (500 results/query). 700 was presented as an alternative with real headroom for a
multi-region/multi-trip master file (the original ask's "spread around the world" framing), still
under the 80% line with BLE active (77.0%).

## Decision

**500.** Chosen over 700 specifically to preserve SRAM headroom for a future feature that isn't
identified yet, rather than to fit any currently-known waypoint count — the user's stated reasoning
was "I want to keep that room in case a new feature comes in that needs that SRAM." Nothing on the
roadmap claims it today, but the 80%-utilization line is a budget for the *whole* project's future,
not just this one feature, and 500 leaves ~32% (68.2% used with BLE active) rather than ~23% (77.0%)
for whatever that turns out to be.

`updateWaypointCountLabel()`'s color thresholds (`src/ui/settings_screen.cpp`) were also fixed in the
same change — they were hardcoded to 30/45 (tuned for cap=50) and would have shown green at
490/500 waypoints loaded. Now proportional: green ≤60% of `MAX_WAYPOINTS`, yellow ≤90%, red above.

## Consequences

**Easier**: a real pocket query (up to 500 results) now loads in full instead of silently truncating.
The render cost per waypoint dropped by close to an order of magnitude regardless of the final cap
chosen, so this headroom decision was cleanly separable from the performance one.

**Harder**: nothing structural — this is a `constexpr` change plus one proportional-threshold fix, not
an architecture change. `docs/waypoint_cap_increase_investigation.md` should be read as historical:
the number it modeled (500) is what shipped, but its "open question for the user" (500 vs. push
further) is answered here, not there.

**Gave up**: a single-PQ export from a Premium account (1,000 results) or a manually merged
multi-region file will still truncate past 500. If that need becomes concrete, the next step is not
"raise the constant again" casually — it's re-running this same SRAM-budget table against whatever
else has claimed space on the roadmap by then, since the 700-940 range was deliberately left
unclaimed rather than unavailable.

**Field verification still open**: per the investigation doc's sequencing, `wpt_us` at a real
500-waypoint load (synthetic GPX or a large real pocket query), fixed-waypoint mode off, has not yet
been measured — only the current real-world waypoint count was field-verified. Boot/parse time via
`loadAllGPXFiles()` at 500 waypoints and a live `memory stats` reading are likewise still open. Worth
doing before calling this fully closed, though the equirectangular rewrite makes a surprise here far
less likely than it was when this ADR's context was written.
