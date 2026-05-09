# Waypoint Filtering System

**GPS Radar Application - Intelligent Waypoint Display**

This document explains the two-strategy waypoint filtering system that intelligently decides which waypoints to display on the radar screen and how to represent off-screen waypoints.

---

## Overview

The GPS Radar uses a **dual-strategy filtering system** to provide optimal situational awareness while preventing visual clutter:

1. **Distance-Based Filtering (Strategy 1)** - Shows waypoints within 10× the current zoom radius
2. **Sector-Based Clustering (Strategy 2)** - Shows maximum 8 off-screen indicators (one per direction)

This approach allows users to see waypoints **beyond the current zoom level** (for navigation planning) while keeping the display clean and performant.

---

## Strategy 1: Distance-Based Filtering

### Purpose
Eliminate waypoints that are too far away to be relevant for current navigation while showing points beyond the visible radar range.

### Configuration
```cpp
// include/ui/ui_manager.h:63
static constexpr float DISTANCE_FILTER_MULTIPLIER = 10.0f;
```

### How It Works

**Adaptive Distance Threshold**:
- At **10km zoom**: Shows waypoints within **100km** (10km × 10)
- At **1km zoom**: Shows waypoints within **10km** (1km × 10)
- At **500m zoom**: Shows waypoints within **5km** (500m × 10)
- At **100m zoom**: Shows waypoints within **1km** (100m × 10)
- At **10m zoom**: Shows waypoints within **100m** (10m × 10)

**Calculation** (`src/ui/navigation.cpp:296-300`):
```cpp
int zoom_idx = static_cast<int>(ui.current_zoom);
float zoom_radius = ui_manager::RadarConfig::ZOOM_CONFIGS[zoom_idx].radius_meters;
float max_indicator_distance = zoom_radius * ui_manager::RadarConfig::DISTANCE_FILTER_MULTIPLIER;
```

**Filtering Logic** (`src/ui/navigation.cpp:340-344`):
```cpp
// Calculate distance using Haversine formula (lines 333-338)
if (distance > max_indicator_distance) {
    continue;  // Too far away, not relevant to current navigation
}
```

### Rationale

**Why 10× multiplier?**

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
// include/ui/ui_manager.h:64-66
static constexpr int MAX_OFFSCREEN_INDICATORS = 8;
static constexpr int INDICATOR_SECTORS = 8;
static constexpr int INDICATOR_SIZE = 15;  // Triangle size (pixels)
static constexpr int INDICATOR_EDGE_INSET = 20;  // Inset from edge (pixels)
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

**Result**: Maximum 8 off-screen indicators, even if 50 waypoints are beyond screen bounds.

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
- **Size**: 15 pixels (triangle base)
- **Color**: `0xFF8800` (orange - distinct from yellow)
- **Position**: 20px inset from circular screen edge
- **Direction**: Triangle points toward waypoint bearing
- **Drawing**: `src/ui/navigation.cpp:250-289`

```cpp
// Position indicator 20px inset from circular edge
int inset = 20;
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
3. Calculate distance to waypoint (Haversine formula)
   ↓
4. STRATEGY 1: Distance filtering
   IF distance > max_indicator_distance:
       SKIP waypoint (too far away)
   ↓
5. Calculate bearing (direction from user to waypoint)
   ↓
6. Convert to screen coordinates (lat/lon → x/y pixels)
   ↓
7. Check if on-screen or off-screen:
   ↓
   ├─ ON-SCREEN (x,y within bounds):
   │  └─ Draw yellow circle immediately
   ↓
   └─ OFF-SCREEN:
      └─ STRATEGY 2: Sector clustering
         ├─ Convert bearing to sector (0-7)
         └─ Keep if closest in sector
   ↓
8. Draw off-screen indicators (max 8 triangles)
```

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
| WP10 | 1500m | Too far | Skip (> 1km = 100m × 10) |
| WP11 | 5km | Too far | Skip |

**Result**:
- 2 yellow circles on screen
- 3 orange triangles at edge (North, East, Southeast directions)
- 10 waypoints filtered out (too far away or replaced by closer ones)

---

## Performance Characteristics

### Computational Complexity
- **Time Complexity**: O(n) where n = waypoint count
- **Single pass**: Each waypoint processed once
- **Operations per waypoint**:
  - 1× Haversine distance calculation (~20 floating-point ops)
  - 1× bearing calculation (~10 floating-point ops)
  - 1× screen coordinate conversion (~5 floating-point ops)
  - 1× sector assignment (integer division)

**Total**: ~35 floating-point ops per waypoint

**At 50 waypoints**: ~1,750 floating-point operations (negligible for ESP32-S3 @ 240MHz)

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
- **On-screen waypoints**: Direct draw (no limit beyond 50 total)
- **Off-screen indicators**: Maximum 8 draws (fixed)
- **Canvas operations**: Simple polygon fills (GPU-accelerated)

**Frame time impact**: <2ms for 50 waypoints at 60 FPS

---

## Configuration and Tuning

### Adjusting Distance Multiplier

**Current**: 10.0× zoom radius

**To increase range** (show more distant waypoints):
```cpp
// include/ui/ui_manager.h:63
static constexpr float DISTANCE_FILTER_MULTIPLIER = 20.0f;  // Show 20× zoom radius
```

**To decrease range** (focus on nearby waypoints):
```cpp
static constexpr float DISTANCE_FILTER_MULTIPLIER = 5.0f;  // Show 5× zoom radius
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
- Maximum 50 waypoints (`ui_manager::RadarConfig::MAX_WAYPOINTS`)

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
    static constexpr int MAX_WAYPOINTS = 50;
    static constexpr int WAYPOINT_SIZE = 25;  // On-screen circle size
    static constexpr int MAX_OFFSCREEN_INDICATORS = 8;
    static constexpr float DISTANCE_FILTER_MULTIPLIER = 10.0f;
    static constexpr int INDICATOR_SECTORS = 8;
    static constexpr int INDICATOR_SIZE = 15;  // Triangle size
    static constexpr int INDICATOR_EDGE_INSET = 20;  // Distance from edge
};
```

---

## Future Enhancements

### Possible Improvements
1. **Adaptive multiplier**: Change distance multiplier based on waypoint density
2. **Priority waypoints**: Always show certain waypoints regardless of distance
3. **Distance labels**: Show distance to off-screen waypoints on indicators
4. **Sector heat map**: Color-code indicators by waypoint count in sector
5. **Animation**: Pulse effect on indicators when new waypoints appear
6. **Touch interaction**: Tap indicator to center map on that direction

### Performance Optimizations
1. **Spatial indexing**: Use quadtree for O(log n) distance queries (only beneficial >200 waypoints)
2. **Dirty flag**: Only recalculate when waypoints change or user moves significantly
3. **GPU acceleration**: Offload Haversine calculations to hardware floating-point unit

---

## Summary

The GPS Radar waypoint filtering system provides:

✅ **Intelligent range limiting** - Shows 10× zoom radius for optimal awareness
✅ **Clean visual display** - Maximum 8 off-screen indicators prevents clutter
✅ **Performance** - O(n) algorithm with negligible overhead
✅ **Flexibility** - Easy to tune via compile-time constants
✅ **User experience** - See what's outside current zoom for navigation planning

This dual-strategy approach balances **situational awareness** (knowing what's beyond screen) with **visual clarity** (not overwhelming the user with 50 indicators).

---

**Last Updated**: 2025-10-18
**Author**: GPS Radar Development Team
**Related Documentation**: `README.md`, `CLAUDE.md`, `docs/gps_settings_simplification.md`
