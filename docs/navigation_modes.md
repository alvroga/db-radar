# Navigation Modes - GPS Radar

**Status**: Complete ✅
**Heading source**: compass (QMC5883L), **not GPS** — see "How It Works" below. This page was
originally written around GPS-heading-fusion (NMEA RMC, 1Hz, unreliable when stationary) and was
rewritten to match the current architecture after that mismatch was found during a documentation
audit — the GPS-based design it originally described was replaced by [ADR-0017](adr/0017-compass-sole-heading-source.md).

## Overview

Two navigation modes control how the radar display orients itself:

1. **Heading-Up Mode** (default) — the radar rotates so you always move "forward" on screen, like
   Google Maps/Waze. Works identically moving or stationary, since heading comes from the compass
   (which reads which way the device is *pointed*), not GPS course-over-ground (which needs motion).
2. **North-Up Mode** — north always points up, fixed orientation, like a paper map or chart plotter.

## How to Switch Modes

Settings > Display > Navigation Mode dropdown. Saved to NVS immediately, no restart required.

## Visual Indicators

**Heading-up mode**, when the north indicator is also enabled in Settings
(`settings_manager::getSettings().north_indicator_enabled` — a separate toggle, not implied by
heading-up mode alone; `navigation.cpp:390-391`): a red circle with white "N", 50px from the screen
edge, rotates to always point toward true north. Example: facing east (heading 90°), the radar
rotates so east points up, and the north indicator appears on the left side (90° counterclockwise
from up).

**North-up mode**: no north indicator (not needed — north is always up); you (red triangle) rotate
as you turn instead of the radar rotating.

## How It Works

**Heading comes solely from the QMC5883L compass**, not GPS. GPS still supplies *position* (UBX
NAV-PVT), but GPS-course heading fusion was removed entirely — see
[ADR-0017](adr/0017-compass-sole-heading-source.md). A compass has no minimum-speed requirement,
which is the whole reason the GPS-heading design was replaced: GPS course-over-ground is genuinely
unreliable at low speed, but a magnetometer works identically standing still or walking.

**Pipeline**:
1. System Task reads the compass at 10Hz, address `0x0D` (`COMPASS_DEVICE`, shared I2C bus — see
   [`i2c.md`](i2c.md)), applies tilt compensation
   ([ADR-0020](adr/0020-tilt-compensation-formula-and-sign-from-bench-data.md)) and WMM declination
   (`docs/wmm_declination.md`), and queues a `COMPASS_UPDATE` with the raw heading.
2. UI Task applies an EMA on receipt (`navigation::smoothHeading`, α=0.15, τ≈0.62s) and writes
   `ui.current_heading`.
3. A 0.5° render deadband on the smoothed value avoids redundant redraws from sub-noise-floor
   jitter while still catching genuine turns — see the `COMPASS_UPDATE` handler in
   `src/utils/task_manager.cpp` (commented in detail there; the threshold has been re-derived twice
   as the compass rate changed).

**Coordinate rotation** (heading-up mode): waypoint position is computed relative to you
(equirectangular approximation, not Haversine — see
[ADR-0022](adr/0022-waypoint-cap-raised-to-500-not-700.md)), then rotated by `-heading` radians
(standard rotation matrix) before being drawn. `rotatePoint()` (`navigation.cpp:255-262`) is the
single-point convenience wrapper; `drawWaypoints()`'s per-frame loop instead computes `cos`/`sin`
once and calls `rotatePointFast(screen_x, screen_y, cos_a, sin_a, center_x, center_y)`
(`navigation.cpp:240-250`) directly per waypoint, avoiding a repeated transcendental call. North
indicator position: `north_x/y = center ± (screen_size/2 - 50) * sin/cos(-heading)`
(`drawNorthIndicator()`, `navigation.cpp`).

**Settings**: `RadarSettings::heading_up_mode` (`include/settings_manager.h`, default `true`),
dropdown in `src/ui/settings_screen.cpp`, loaded at startup in `src/ui/ui_manager.cpp`.

### No Stationary Fallback (By Design)

Earlier revisions of this radar (and of this doc) had a 10-second stationary timeout that reverted
heading-up mode to north-up, because GPS course-over-ground couldn't be trusted at rest. **That
mechanism doesn't exist in the current code** — `ui.last_valid_heading` and `ui.last_heading_update`
are still declared (`include/ui/ui_manager.h`) but nothing writes to them anymore; they're dead
fields left over from the removed design, not a subtle behavior to rediscover.

## Performance

- **Per-waypoint overhead**: equirectangular dx/dy + `sqrtf` (rotation on top), replacing the earlier
  Haversine — see [ADR-0022](adr/0022-waypoint-cap-raised-to-500-not-700.md). At the current
  `MAX_WAYPOINTS` (200, working-set size — see CLAUDE.md's Waypoint Two-Tier Index section), see
  `docs/waypoint_filtering.md` for the up-to-date per-waypoint cost breakdown; read `wpt_us` off the
  `perf` HUD for the current measured figure rather than trusting a fixed estimate.
- **Heading update rate**: 10Hz (compass), independent of GPS fix rate. No heap allocation during
  rotation (stack-based).
- **Compass read cost**: one I2C transaction per 100ms sample, negligible power impact — pure
  calculation, no radio.

## Troubleshooting

**North indicator not showing**: confirm Settings > Display > Navigation Mode is Heading-Up (the
indicator only appears in that mode). If it's selected and still missing, this points to a compass
read/calibration issue, not GPS — see `docs/compass.md` and `docs/compass_calibration_foundation.md`.

**Radar rotates erratically**:
1. **Compass calibration** — an uncalibrated or recently-disturbed compass (see
   `compass_calibration_foundation.md` for when recalibration is needed — even opening the enclosure
   can invalidate it) is the most likely cause now that heading doesn't come from GPS at all.
2. **Fast tilt transitions** — a quick flat→vertical tilt can produce a brief (~1s, self-correcting)
   heading bounce from the gravity-EMA lag; accepted behavior, see ROADMAP.md's Resolved section.
3. **Magnetic interference** — nearby ferrous metal or electronics can distort the local field.

## Not Planned

**Waypoint-up mode** (rotate to always show the next waypoint "ahead," for single-waypoint hiking)
was considered and rejected — heading-up mode covers the same use case well enough.

## Reference Documentation

- **Architecture summary**: CLAUDE.md's Navigation Modes System section
- **Heading source decision**: [ADR-0017](adr/0017-compass-sole-heading-source.md)
- **Tilt compensation**: [ADR-0020](adr/0020-tilt-compensation-formula-and-sign-from-bench-data.md), `docs/compass_calibration_foundation.md`
