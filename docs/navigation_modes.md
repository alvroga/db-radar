# Navigation Modes - GPS Radar

**Status**: Complete ✅
**Heading source**: compass (QMC5883L), **not GPS** — see "How It Works" below. This page was
originally written around GPS-heading-fusion (NMEA RMC, 1Hz, unreliable when stationary) and was
rewritten to match the current architecture after that mismatch was found during a documentation
audit — the GPS-based design it originally described was replaced by [ADR-0017](adr/0017-compass-sole-heading-source.md).

## Overview

The GPS Radar supports **two navigation modes** that control how the radar display orients itself:

1. **Heading-Up Mode** (Default) - The radar rotates so you always move "forward" on the screen
2. **North-Up Mode** (Classic) - North always points to the top of the screen (fixed orientation)

Think of it like Google Maps:
- **Heading-Up** = "Rotate map with my direction" (3D navigation view)
- **North-Up** = "Lock map to north" (traditional paper map view)

---

## Quick Comparison

| Feature | Heading-Up Mode | North-Up Mode |
|---------|----------------|---------------|
| **Orientation** | Rotates with your compass heading | Fixed (north always up) |
| **Best For** | Active navigation, walking to waypoints | Map reading, planning routes |
| **Cognitive Load** | Low (you always move "forward") | Higher (requires mental rotation) |
| **North Indicator** | Red circle with "N" shows where north is | Not shown (north is always up) |
| **When Stationary** | Works identically — a compass doesn't need motion | Always shows north |
| **Industry Standard** | Google Maps, Waze, car GPS default | Hiking GPS, aviation charts, marine charts |

---

## When to Use Each Mode

### Use Heading-Up Mode For:

✅ **Active Navigation**
When walking/driving to a waypoint - you always move "forward" on the radar, making it intuitive to know which way to turn.

✅ **Urban Navigation**
In cities with complex street layouts - the radar matches your actual view of the world.

✅ **Route Following**
When following a path with multiple waypoints - easier to see "next waypoint is ahead and to the right" without mental rotation.

✅ **Beginner Users**
More intuitive for users unfamiliar with traditional map reading.

**Example Scenario**:
"I'm facing east toward a waypoint 200m away. On the radar, the waypoint appears directly ahead. When I turn left to face north, the radar rotates and the waypoint now appears to my right. I always know my relative position without thinking — this works exactly the same whether I'm walking or standing still, since the compass tracks which way I'm *facing*, not which way I'm *moving*."

---

### Use North-Up Mode For:

✅ **Map Reading**
When comparing radar to a paper map or satellite imagery - both use the same north-up orientation.

✅ **Route Planning**
When studying waypoint positions before moving - easier to understand absolute geographic relationships.

✅ **Orientation Reference**
When you need to know absolute cardinal directions (N/S/E/W) at a glance.

✅ **Surveying/Professional Use**
When documenting locations or coordinates where absolute orientation matters.

**Example Scenario**:
"I have three waypoints forming a triangle north of my position. I want to plan which order to visit them. North-up mode lets me see their absolute positions without the display rotating as I turn around."

---

## How to Switch Modes

### Via Settings Screen

1. Touch the screen or press GPIO0 button to open settings
2. Navigate to **Display** tab
3. Find **Navigation Mode** dropdown
4. Select:
   - **Heading-Up** - Radar rotates with your compass heading (default)
   - **North-Up** - North always points up (classic mode)
5. Setting is saved to NVS (persists across reboots)
6. Radar updates immediately (no restart required)

**Settings Path**: Settings > Display > Navigation Mode

---

## Visual Indicators

### Heading-Up Mode

When heading-up mode is active, a **north indicator** appears on the radar:

- **Appearance**: Red circle with white "N" letter
- **Position**: 50 pixels from screen edge
- **Behavior**: Rotates to always point toward true north
- **Purpose**: Shows absolute orientation while radar rotates with heading

**Why It's Needed**:
In heading-up mode, the radar rotates so "up" matches your compass heading. The north indicator lets you know where true north is at any time.

**Example**:
- You're facing **east** (heading = 90°)
- Radar rotates so **east points up**
- North indicator appears on the **left side** of the screen (90° counterclockwise from up)

### North-Up Mode

- **No north indicator** (not needed - north is always up)
- Radar orientation is fixed
- You (red triangle) rotate as you turn

---

## How It Works

### Compass Heading Data

**Heading comes solely from the QMC5883L compass**, not GPS. GPS still supplies *position* (over UBX
NAV-PVT, not NMEA), but heading fusion from GPS course-over-ground was removed entirely — see
[ADR-0017](adr/0017-compass-sole-heading-source.md).

- The System Task reads the compass at **10Hz** and queues a `COMPASS_UPDATE` for the UI Task, which
  applies an EMA (`navigation::smoothHeading`, α=0.15, τ≈0.62s) and writes `ui.current_heading`.
- Tilt compensation (accelerometer-based, not gyro) corrects the heading past ~31.5° of tilt — see
  [ADR-0020](adr/0020-tilt-compensation-formula-and-sign-from-bench-data.md) and `docs/compass.md`.
- **A compass has no minimum-speed requirement.** This is the whole reason the GPS-heading design was
  replaced: GPS course-over-ground genuinely is unreliable at low speed (no position delta to derive
  direction from), but a magnetometer measures which way the device is *pointed*, which works
  identically standing still or walking.

**Why this matters for navigation mode design**: the original design's entire "stationary fallback"
mechanism (below) existed to paper over GPS heading's stationary weakness. With a compass, that
weakness doesn't exist, so the fallback isn't needed and isn't present in the current code.

---

### Coordinate Rotation Algorithm

In heading-up mode, the radar performs **2D coordinate rotation**:

1. Calculate waypoint position relative to you (equirectangular approximation, not Haversine — see
   [ADR-0022](adr/0022-waypoint-cap-raised-to-500-not-700.md))
2. Rotate coordinates by `-heading` radians (counterclockwise)
3. Display rotated position on screen

**Math**: Standard rotation matrix:
```
x' = x * cos(-θ) - y * sin(-θ)
y' = x * sin(-θ) + y * cos(-θ)
```

Where `θ = heading in radians`

**Result**: If your heading is 90° (east), the map rotates -90° so east points "up" on the screen.

---

### No Stationary Fallback (By Design)

Earlier revisions of this radar (and of this doc) had a 10-second stationary timeout that reverted
heading-up mode to north-up, because GPS course-over-ground couldn't be trusted at rest. **That
mechanism doesn't exist in the current code** — `ui.last_valid_heading` and `ui.last_heading_update`
are still declared (`include/ui/ui_manager.h`) but nothing writes to them anymore; they're dead
fields left over from the removed design, not a subtle behavior to rediscover.

Heading-up mode now simply tracks `ui.current_heading` continuously, moving or not, because the
compass supplies a valid reading either way.

---

## Performance

### Computational Cost

- **Per-waypoint overhead**: equirectangular dx/dy + `sqrtf` (rotation on top), replacing the earlier
  Haversine — see [ADR-0022](adr/0022-waypoint-cap-raised-to-500-not-700.md), 2026-08-05
- **At the current `MAX_WAYPOINTS` (200, working-set size — see the Waypoint Two-Tier Index section of
  CLAUDE.md)**: see `docs/waypoint_filtering.md` for the up-to-date per-waypoint cost breakdown
- **Frame time**: read `wpt_us` off the `perf` HUD for the current measured figure rather than
  trusting a fixed estimate
- **Heading update rate**: 10Hz (compass), independent of GPS fix rate

### Memory Usage

- **RAM**: heading state lives in `g_ui_state` (`current_heading`, plus the now-dead
  `last_valid_heading`/`last_heading_update` fields noted above)
- **No heap allocation** during rotation (stack-based calculations)

### Power Impact

- **Negligible**: rotation is pure calculation (no I/O, no radio)
- **Compass read cost**: one I2C transaction per 100ms sample (10Hz), through the shared bus — see
  [`i2c.md`](i2c.md)

---

## Troubleshooting

### "North indicator not showing"

**Check navigation mode setting**:
1. Open Settings > Display
2. Verify **Navigation Mode** is set to **Heading-Up**
3. North indicator only appears in heading-up mode (not needed in north-up)

If heading-up is selected and the indicator is still missing, this points to a compass read/calibration
issue rather than a GPS one — see `docs/compass.md` and `docs/compass_calibration_foundation.md`.

---

### "Radar rotates erratically"

**Causes**:

1. **Compass calibration** — an uncalibrated or recently-disturbed compass (see
   [`compass_calibration_foundation.md`](compass_calibration_foundation.md) for when recalibration is
   needed — even opening the enclosure can invalidate it) is the most likely cause now that heading
   doesn't come from GPS at all.
2. **Fast tilt transitions** — a quick flat→vertical tilt can produce a brief (~1s, self-correcting)
   heading bounce from the gravity-EMA lag; accepted behavior, see CLAUDE.md's Compass Calibration &
   Tilt entry in ROADMAP.md's Resolved section.
3. **Magnetic interference** — nearby ferrous metal or electronics can distort the local field.

---

## Industry Examples

### Why Both Modes Are Standard

**Aviation**:
- Commercial aircraft GPS: Track-Up, Heading-Up, AND North-Up modes
- Pilots switch based on phase of flight (navigation vs planning)

**Marine**:
- Chart plotters: Often default to North-Up for chart correlation
- Active navigation mode available for route following

**Automotive**:
- Car GPS: Almost always heading-up (Waze, Google Maps default)
- North-up available for users who prefer it

**Hiking GPS**:
- Garmin devices: Often default to North-Up (map reading focus)
- Track-Up mode for active trail following

**Consumer Apps**:
- Google Maps: Heading-up default, north-up toggle available
- Apple Maps: Same dual-mode approach
- Waze: Heading-up only (pure navigation focus)

**Professional Use**:
- Surveyors: North-up for absolute position documentation
- Search & Rescue: Both modes depending on task
- Military: All three modes (north-up, heading-up, waypoint-up)

**Conclusion**: Dual-mode support is not optional - it's industry best practice across all navigation domains.

---

## Technical Implementation

For developers and those curious about how it works under the hood.

### Compass Heading Pipeline

**Source**: QMC5883L on the shared I2C bus, address `0x0D` (`COMPASS_DEVICE`) — see [`i2c.md`](i2c.md).

**Read + smoothing flow**:
1. System Task reads the compass at 10Hz, applies tilt compensation
   ([ADR-0020](adr/0020-tilt-compensation-formula-and-sign-from-bench-data.md)) and WMM declination
   (`docs/wmm_declination.md`), and queues a `COMPASS_UPDATE` with the raw heading.
2. UI Task applies an EMA (`navigation::smoothHeading`, α=0.15, τ≈0.62s) on receipt and writes
   `ui.current_heading`.
3. A small render deadband (0.5°) on the *smoothed* value avoids redundant redraws from sub-noise-floor
   jitter while still catching genuine turns — see the `COMPASS_UPDATE` handler in
   `src/utils/task_manager.cpp` for the full reasoning (it's commented in detail there since the
   threshold has been re-derived twice as the compass rate changed).

**Code Location**: `src/utils/task_manager.cpp` — `COMPASS_UPDATE` case; `src/ui/navigation.cpp` —
`smoothHeading()`

---

### Coordinate Rotation Function

**Code Location**: `src/ui/navigation.cpp:98-118`

**Algorithm**:
```cpp
void rotatePoint(int& screen_x, int& screen_y, float heading, int center_x, int center_y) {
    // 1. Translate to origin (center of radar)
    int rel_x = screen_x - center_x;
    int rel_y = screen_y - center_y;

    // 2. Rotate by -heading (counterclockwise) to make heading point "up"
    float angle_rad = -heading * M_PI / 180.0f;
    float cos_a = cos(angle_rad);
    float sin_a = sin(angle_rad);

    int rotated_x = (int)(rel_x * cos_a - rel_y * sin_a);
    int rotated_y = (int)(rel_x * sin_a + rel_y * cos_a);

    // 3. Translate back
    screen_x = rotated_x + center_x;
    screen_y = rotated_y + center_y;
}
```

**Applied In**: `src/ui/navigation.cpp` (inside `latLonToScreen()`)

---

### North Indicator Rendering

**Code Location**: `src/ui/navigation.cpp` — `drawNorthIndicator()`

**Drawing Process**:
1. Calculate north position relative to current heading
2. Draw red filled circle (30px diameter) via thick arc
3. Draw white "N" text centered on circle
4. Only drawn when `heading_up_mode == true`

**Position Calculation**:
```cpp
int north_distance = screen_size / 2 - 50;  // 50px from edge
float north_angle = -ui.current_heading * M_PI / 180.0f;
int north_x = center_x + (int)(north_distance * sin(north_angle));
int north_y = center_y - (int)(north_distance * cos(north_angle));
```

---

### Settings Integration

**NVS Persistence**: `include/settings_manager.h`
```cpp
struct RadarSettings {
    // ...
    bool heading_up_mode = true;  // Default: heading-up (user preference)
    // ...
};
```

**Settings UI**: `src/ui/settings_screen.cpp`
- Dropdown with options: "Heading-Up" / "North-Up"
- Immediate effect (no restart required)
- Saves to NVS on change

**Startup Loading**: `src/ui/ui_manager.cpp`
```cpp
settings_manager::RadarSettings settings;
settings_manager::loadSettings(settings);
g_ui_state.heading_up_mode = settings.heading_up_mode;  // Apply from NVS
```

---

## Future Enhancements

### Waypoint-Up Mode (Advanced)

**Concept**: Radar rotates to always show next waypoint "ahead"

**Use Case**: Single-waypoint navigation (hiking to one destination)

**Implementation**: Similar to heading-up, but rotation based on bearing to waypoint instead of compass heading

**Status**: Not planned (heading-up mode covers most use cases)

---

## Reference Documentation

- **Architecture Details**: See CLAUDE.md's Navigation Modes System section
- **Heading source decision**: [ADR-0017](adr/0017-compass-sole-heading-source.md)
- **Tilt compensation**: [ADR-0020](adr/0020-tilt-compensation-formula-and-sign-from-bench-data.md), `docs/compass_calibration_foundation.md`
- **Code References**: All locations listed in Technical Implementation section above
