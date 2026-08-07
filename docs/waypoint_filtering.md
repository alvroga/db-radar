# Waypoint Filtering System

**GPS Radar Application - Intelligent Waypoint Display**

This document explains the two-strategy waypoint filtering system that intelligently decides which waypoints to display on the radar screen and how to represent off-screen waypoints.

---

## Overview

The GPS Radar uses a **dual-strategy filtering system** to provide optimal situational awareness while preventing visual clutter:

1. **Distance-Based Filtering (Strategy 1)** - Shows waypoints within 100× the current zoom radius
2. **Sector-Based Clustering (Strategy 2)** - Shows maximum 8 off-screen indicators (one per direction)

This approach allows users to see waypoints **beyond the current zoom level** (for navigation planning) while keeping the display clean and performant.

---

## Strategy 1: Distance-Based Filtering

### Purpose
Eliminate waypoints that are too far away to be relevant for current navigation while showing points beyond the visible radar range.

### Configuration
```cpp
// include/ui/ui_manager.h — RadarConfig
static constexpr float DISTANCE_FILTER_MULTIPLIER = 100.0f;  // raised from 10.0f, 2026-08-07
```

### How It Works

**Adaptive Distance Threshold** (multiplier raised 10× → 100×, 2026-08-07 — see
[Touch Interaction and Distance Display](#touch-interaction-and-distance-display) below for why):
- At **1km zoom**: Shows waypoints within **100km** (1km × 100)
- At **500m zoom**: Shows waypoints within **50km** (500m × 100)
- At **200m zoom**: Shows waypoints within **20km** (200m × 100)
- At **100m zoom**: Shows waypoints within **10km** (100m × 100)
- At **50m zoom**: Shows waypoints within **5km** (50m × 100)

Raising the multiplier is safe because sector clustering (Strategy 2, below) already caps visible
off-screen triangles at `MAX_OFFSCREEN_INDICATORS` (8) regardless of how many candidates pass this
filter — a wider cutoff only changes which waypoint fills a sector when nothing closer already exists
in it. It's also meaningful, not just harmless: the working-set selection
(`gpx_loader::selectAndMaterialize()`/`reselect()`, see [ADR-0023](adr/0023-two-tier-waypoint-index.md))
picks the N globally-closest waypoints with **no distance cap**, so there are real far-away candidates
for a wider cutoff to actually surface.

**Calculation** (`src/ui/navigation.cpp:296-300`):
```cpp
int zoom_idx = static_cast<int>(ui.current_zoom);
float zoom_radius = ui_manager::RadarConfig::ZOOM_CONFIGS[zoom_idx].radius_meters;
float max_indicator_distance = zoom_radius * ui_manager::RadarConfig::DISTANCE_FILTER_MULTIPLIER;
```

**Filtering Logic** (`src/ui/navigation.cpp:672-678`):
```cpp
// Distance from the equirectangular approximation (see below), not Haversine
if (distance > max_indicator_distance) {
    continue;  // Too far away, not relevant to current navigation
}
```

### Rationale

**Why 100× multiplier?**

1. **Navigation Planning**: See waypoints outside current zoom radius to plan route
2. **Situational Awareness**: Know what's ahead without constantly changing zoom
3. **Performance**: Eliminates waypoints thousands of km away (e.g., different continents)
4. **User Experience**: Balance between "too many indicators" and "missing context"

**Example Scenario**:
- User is navigating at 1km zoom (street/neighborhood level)
- System shows waypoints within 10km radius
- Off-screen indicators point to destinations in nearby areas
- User can zoom out to 10km to see all waypoints on-screen

---

## Strategy 2: Sector-Based Clustering

### Purpose
Prevent off-screen indicator clutter by showing **maximum 8 directional indicators** (one per compass direction).

### Configuration
```cpp
// include/ui/ui_manager.h — RadarConfig
static constexpr int MAX_OFFSCREEN_INDICATORS = 8;
static constexpr int INDICATOR_SECTORS = 8;
static constexpr int INDICATOR_SIZE = 25;  // Triangle size (pixels)
static constexpr int INDICATOR_EDGE_INSET = 25;  // Inset from edge (pixels)
```

### How It Works

**8-Sector Division** (`src/ui/navigation.cpp:303-310`):
```
       N (0)
   NW (7)  NE (1)
W (6)         E (2)
   SW (5)  SE (3)
       S (4)
```

Each sector covers 45° of the compass:
- **Sector 0 (N)**: 337.5° - 22.5° (North)
- **Sector 1 (NE)**: 22.5° - 67.5° (Northeast)
- **Sector 2 (E)**: 67.5° - 112.5° (East)
- **Sector 3 (SE)**: 112.5° - 157.5° (Southeast)
- **Sector 4 (S)**: 157.5° - 202.5° (South)
- **Sector 5 (SW)**: 202.5° - 247.5° (Southwest)
- **Sector 6 (W)**: 247.5° - 292.5° (West)
- **Sector 7 (NW)**: 292.5° - 337.5° (Northwest)

**Clustering Algorithm** (`src/ui/navigation.cpp:371-386`):

For each off-screen waypoint:
1. Calculate bearing (direction from user to waypoint)
2. Convert bearing to sector index (0-7)
3. **Keep only the CLOSEST waypoint per sector**
4. Replace sector's waypoint if new one is closer

```cpp
// Convert bearing (-π to π) to sector index (0-7)
float bearing_deg = bearing * 180.0f / M_PI_LOCAL;
if (bearing_deg < 0) bearing_deg += 360.0f;

int sector = (int)((bearing_deg + 22.5f) / 45.0f) % NUM_SECTORS;

// Keep closest waypoint per sector
if (distance < sectors[sector].closest_distance) {
    sectors[sector].has_waypoint = true;
    sectors[sector].closest_distance = distance;
    sectors[sector].bearing = bearing;
}
```

**Result**: Maximum 8 off-screen indicators, even if hundreds of waypoints are beyond screen bounds
(`MAX_WAYPOINTS` is 200 — a working-set size, not a device-wide cap; see the two-tier PSRAM index,
[ADR-0023](adr/0023-two-tier-waypoint-index.md)).

---

## Visual Indicators

### On-Screen Waypoints
**Appearance**: Yellow filled circles
- **Size**: 25×25 pixels (circular)
- **Color**: `0xFFFF00` (bright yellow)
- **Drawing**: `src/ui/navigation.cpp:365-369`

```cpp
if (x >= 0 && x < screen_size && y >= 0 && y < screen_size) {
    // On-screen: draw yellow circle beacon
    int size = ui_manager::RadarConfig::WAYPOINT_SIZE;  // 25x25
    int half_size = size / 2;
    lv_canvas_draw_rect(canvas, x - half_size, y - half_size, size, size, &circle_dsc);
}
```

### Off-Screen Indicators
**Appearance**: Orange triangles at screen edge
- **Size**: 25 pixels (triangle base)
- **Color**: `0xFF8800` (orange - distinct from yellow)
- **Position**: 25px inset from circular screen edge
- **Direction**: Triangle points toward waypoint bearing
- **Drawing**: `src/ui/navigation.cpp:250-289`

```cpp
// Position indicator 25px inset from circular edge
int inset = 25;
float edge_x = center_x + (radius - inset) * sin(bearing);
float edge_y = center_y - (radius - inset) * cos(bearing);

// Triangle points outward toward waypoint direction
points[0].x = edge_x + (tri_size * 0.8) * sin(bearing);  // Tip
points[0].y = edge_y - (tri_size * 0.8) * cos(bearing);
// ... left and right base points
```

**Why Orange?**
- Distinct from yellow on-screen beacons (easy differentiation)
- High visibility against green radar background
- Clear indication: "waypoint exists in this direction, but off-screen"

---

## Complete Filtering Pipeline

### Processing Flow (`src/ui/navigation.cpp:291-395`)

```
1. Calculate max_indicator_distance (zoom_radius × 10)
   ↓
2. FOR each waypoint (0 to waypoint_count):
   ↓
3. Convert lat/lon delta to dx/dy meters (equirectangular approximation) and
   derive distance = sqrtf(dx² + dy²) — screen x/y fall out of the same dx/dy,
   no separate "convert to screen coordinates" step
   ↓
4. STRATEGY 1: Distance filtering
   IF distance > max_indicator_distance:
       SKIP waypoint (too far away)
   ↓
5. Check if on-screen or off-screen:
   ↓
   ├─ ON-SCREEN (x,y within bounds):
   │  └─ Draw yellow circle immediately (bearing never computed)
   ↓
   └─ OFF-SCREEN:
      └─ Calculate bearing (atan2f, only reached for off-screen waypoints)
         └─ STRATEGY 2: Sector clustering
            ├─ Convert bearing to sector (0-7)
            └─ Keep if closest in sector
   ↓
6. Draw off-screen indicators (max 8 triangles)
```

**Note**: as of the 2026-08-05 rewrite, bearing (`atan2f`) is computed only for waypoints that end up
off-screen — on-screen waypoints never need it. This is a change from the diagram's earlier "always
compute bearing" step. See [ADR-0022](adr/0022-waypoint-cap-raised-to-500-not-700.md).

### Example Execution

**Scenario**: User at 100m zoom with 15 waypoints

| Waypoint | Distance | Status | Action |
|----------|----------|--------|--------|
| WP1 | 50m | On-screen | Draw yellow circle at (x, y) |
| WP2 | 80m | On-screen | Draw yellow circle at (x, y) |
| WP3 | 150m | Off-screen, Sector N | Add to sector 0 (North) |
| WP4 | 200m | Off-screen, Sector N | Replace sector 0 (closer) |
| WP5 | 300m | Off-screen, Sector E | Add to sector 2 (East) |
| WP6 | 500m | Off-screen, Sector SE | Add to sector 3 (Southeast) |
| ... | ... | ... | ... |
| WP10 | 15km | Too far | Skip (> 10km = 100m × 100) |
| WP11 | 50km | Too far | Skip |

**Result**:
- 2 yellow circles on screen
- 3 orange triangles at edge (North, East, Southeast directions)
- 10 waypoints filtered out (too far away or replaced by closer ones)

---

## Performance Characteristics

### Computational Complexity
- **Time Complexity**: O(n) where n = waypoint count
- **Single pass**: Each waypoint processed once
- **Operations per waypoint** (as of the 2026-08-05 Haversine → equirectangular rewrite,
  [ADR-0022](adr/0022-waypoint-cap-raised-to-500-not-700.md)):
  - 1× equirectangular dx/dy conversion (`dx = R·Δlon·cos(lat)`, `dy = R·Δlat`) + `sqrtf` for
    distance — 2 multiplies + 1 sqrt, no double-precision transcendentals, replacing the prior
    Haversine's 10 double `sin`/`cos`/`atan2`/`sqrt` calls/waypoint
  - 1× bearing calculation (`atan2f`) — **only for waypoints that end up off-screen**; on-screen
    waypoints skip it entirely
  - 1× sector assignment (integer division) — off-screen waypoints only

**At the current `MAX_WAYPOINTS = 500`** this is the change that made raising the cap from 50 safe —
the old per-waypoint cost was unconditional double-precision transcendentals on an ESP32-S3 FPU that's
single-precision only, so it would have scaled 10× worse at the new cap. Field-verified on hardware at
the current real-world waypoint count; a synthetic 500-waypoint `wpt_us` measurement is still open
(see ADR-0022).

### Memory Usage
- **Sector storage**: 8 × `SectorWaypoint` structs
  ```cpp
  struct SectorWaypoint {
      bool has_waypoint = false;      // 1 byte
      float closest_distance = FLT_MAX; // 4 bytes
      double bearing = 0.0;            // 8 bytes
  };  // Total: 13 bytes + 3 padding = 16 bytes
  ```
- **Total**: 8 × 16 bytes = **128 bytes** (stack allocation)

### Rendering Performance
- **On-screen waypoints**: Direct draw (no limit beyond `MAX_WAYPOINTS` total)
- **Off-screen indicators**: Maximum 8 draws (fixed)
- **Canvas operations**: Simple polygon fills (GPU-accelerated)

**Frame time impact**: the often-quoted "<2ms for 50 waypoints" figure was never actually measured —
see CLAUDE.md's Waypoint Filtering section. Waypoint drawing measures ~5ms in the instrumented frame
breakdown at the old 50-waypoint cap. Read `wpt_us` off the `perf` HUD for the current figure rather
than trusting either number, especially at the new 500-waypoint cap.

---

## Configuration and Tuning

### Adjusting Distance Multiplier

**Current**: 100.0× zoom radius (raised from 10.0×, 2026-08-07)

**To increase range** (show more distant waypoints):
```cpp
// include/ui/ui_manager.h — RadarConfig
static constexpr float DISTANCE_FILTER_MULTIPLIER = 200.0f;  // Show 200× zoom radius
```

**To decrease range** (focus on nearby waypoints):
```cpp
static constexpr float DISTANCE_FILTER_MULTIPLIER = 10.0f;  // Show 10× zoom radius (the old default)
```

**Trade-offs**:
- **Higher multiplier**: More situational awareness, more off-screen indicators
- **Lower multiplier**: Tighter focus, fewer distractions

### Changing Sector Count

**Current**: 8 sectors (cardinal + intercardinal directions)

**To increase precision** (16 sectors - every 22.5°):
```cpp
// include/ui/ui_manager.h:64
static constexpr int INDICATOR_SECTORS = 16;
static constexpr int MAX_OFFSCREEN_INDICATORS = 16;
```

**Note**: Must update sector calculation in `navigation.cpp:378`:
```cpp
int sector = (int)((bearing_deg + 11.25f) / 22.5f) % NUM_SECTORS;
```

**Trade-offs**:
- **More sectors**: Higher precision, more indicators (may clutter edge)
- **Fewer sectors**: Cleaner display, less precise direction info

---

## Integration with Other Systems

### Zoom System
- Filtering automatically adapts to zoom level changes
- No configuration needed - uses `ui.current_zoom` state
- Zoom levels defined in `ui_manager.h:71-77`

### GPX Loader
- Filtering operates on `ui.waypoints[]` array
- GPX loader populates array via `gpx_loader::loadAllGPXFiles()`
- Working-set size of 200 (`ui_manager::RadarConfig::MAX_WAYPOINTS`) — the closest N waypoints across
  every indexed GPX file, reselected on movement; see the two-tier PSRAM index,
  [ADR-0023](adr/0023-two-tier-waypoint-index.md)

### Radar Display
- Called by `navigation::updateRadarDisplay()` every frame
- Integrated with grid drawing and center triangle
- Respects circular screen clipping

---

## Code References

### Key Files
- **Algorithm**: `src/ui/navigation.cpp:291-395` - `drawWaypoints()` function
- **Configuration**: `include/ui/ui_manager.h:54-81` - `RadarConfig` struct
- **Off-screen drawing**: `src/ui/navigation.cpp:250-289` - `drawOffScreenIndicator()`
- **Coordinate conversion**: `src/ui/navigation.cpp:98-135` - `latLonToScreen()`

### Important Constants
```cpp
// include/ui/ui_manager.h
struct RadarConfig {
    // 500 (ADR-0022) failed to boot on hardware the same day and was rolled back to 200;
    // the two-tier PSRAM index (ADR-0023) is what actually solved "more waypoints than
    // fit in the working set" — this is a working-set size, not a device-wide cap.
    static constexpr int MAX_WAYPOINTS = 200;
    static constexpr int WAYPOINT_SIZE = 25;  // On-screen circle size
    static constexpr int MAX_OFFSCREEN_INDICATORS = 8;
    static constexpr float DISTANCE_FILTER_MULTIPLIER = 100.0f;  // raised from 10.0f, 2026-08-07
    static constexpr int INDICATOR_SECTORS = 8;
    static constexpr int INDICATOR_SIZE = 25;  // Triangle size
    static constexpr int INDICATOR_EDGE_INSET = 25;  // Distance from edge
    static constexpr float FIXED_WAYPOINT_MAX_DISTANCE_M = 100000.0f;  // Auto-unfix past this range
};
```

---

## Touch Interaction and Distance Display

**Status**: Implemented 2026-08-07, extended same day, build-verified (not yet field-tested)

Off-screen indicators are tappable, same as on-screen waypoint dots. `drawWaypoints()`
(`navigation.cpp`) persists each drawn indicator's screen position, source waypoint index, and
distance into a file-static `g_offscreen_tap[]` array (one slot per sector + one for the fixed
waypoint) every frame; `handleTapAt()` hit-tests against it (24px radius) after checking on-screen
dots, and opens the same waypoint detail screen a tapped on-screen dot would. No mutex is needed —
both the draw callback and the touch callback run on the UI Task.

There are two distinct distance displays, serving two different purposes — an early version of this
feature conflated them, so the distinction is worth stating explicitly:

- **One-shot glance, no fixing required**: the waypoint detail screen (`waypoint_screen.cpp`) shows a
  DISTANCE row (Haversine via `utils/geo.h`, not this doc's equirectangular approximation — a tapped
  off-screen waypoint can be well outside that approximation's accurate range), formatted as meters
  under 1km and `"%.1f km"` at or above it. Computed once when the screen opens — cheap (a single
  Haversine call), not a per-frame cost. Hidden without a GPS fix.
- **Live tracking while fixed**: the existing middle-left `waypoint_distance_label` ("Fixed: Xm"/"Fixed:
  X.X km") and its `fixed_waypoint_icon` (a small dot-in-ring canvas above the label, matching the
  on-radar waypoint beacon's own color, tap target for the same action) update continuously while a
  waypoint is fixed — same widgets whether the fixed waypoint is on-screen or off-screen. Previously
  this auto-unfixed at a hardcoded 1km regardless of how far the fixed waypoint was, which made fixing
  anything off-screen effectively non-functional (it would fix and immediately auto-release next
  frame). The cap is now `RadarConfig::FIXED_WAYPOINT_MAX_DISTANCE_M` (100km, raised from an initial
  20km the same day — 20km was itself still blocking legitimate fixes on distant off-screen waypoints,
  and 100km now matches `DISTANCE_FILTER_MULTIPLIER`'s own cutoff at 1km zoom) — a safety net for a
  stale fix, not a normal-use limit — and the label formats km above 1000m like the detail screen's
  DISTANCE row does.

`drawWaypoints()` also still enforces "when a waypoint is fixed, render only that target" — all other
on-screen dots and off-screen triangles disappear, on-screen or off.

## Future Enhancements

### Possible Improvements
1. **Adaptive multiplier**: Change distance multiplier based on waypoint density
2. **Priority waypoints**: Always show certain waypoints regardless of distance
3. **Sector heat map**: Color-code indicators by waypoint count in sector
4. **Animation**: Pulse effect on indicators when new waypoints appear

### Performance Optimizations
1. **Spatial indexing**: Use quadtree for O(log n) distance queries (beneficial well before the
   current 500-waypoint cap, unlike when this was written against a 50-waypoint cap)
2. **Dirty flag**: Only recalculate when waypoints change or user moves significantly
3. ~~GPU acceleration: Offload Haversine calculations to hardware floating-point unit~~ — moot;
   Haversine was replaced by the equirectangular approximation (2026-08-05), which is already cheap
   enough on the single-precision FPU that no further offload is being considered

---

## Summary

The GPS Radar waypoint filtering system provides:

✅ **Intelligent range limiting** - Shows 100× zoom radius for optimal awareness
✅ **Clean visual display** - Maximum 8 off-screen indicators prevents clutter
✅ **Performance** - O(n) algorithm with negligible overhead
✅ **Flexibility** - Easy to tune via compile-time constants
✅ **User experience** - See what's outside current zoom for navigation planning

This dual-strategy approach balances **situational awareness** (knowing what's beyond screen) with **visual clarity** (not overwhelming the user with hundreds of indicators).

---

**Last Updated**: 2026-08-07 (`DISTANCE_FILTER_MULTIPLIER` raised 10×→100×; fixed-waypoint live distance
label's auto-unfix cap raised 1km→`FIXED_WAYPOINT_MAX_DISTANCE_M`, 20km then same-day 100km, so fixing
off-screen waypoints actually works; added `fixed_waypoint_icon` — see Touch Interaction section above).
Same day, earlier:
off-screen indicators made tappable, detail-screen distance display added. Previously 2026-08-05
(`MAX_WAYPOINTS` raised 50 → 500, then rolled back to 200 same day after a hardware boot failure — see
[ADR-0022](adr/0022-waypoint-cap-raised-to-500-not-700.md) — superseded by the two-tier PSRAM index,
[ADR-0023](adr/0023-two-tier-waypoint-index.md); Haversine replaced with equirectangular approximation)
**Author**: GPS Radar Development Team
**Related Documentation**: `README.md`, `CLAUDE.md`, `docs/gps_settings_simplification.md`
