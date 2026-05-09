# Navigation Modes - GPS Radar

**Status**: Complete ✅
**Implemented**: 2025-10-20
**Version**: v0.10.0

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
| **Orientation** | Rotates with your walking direction | Fixed (north always up) |
| **Best For** | Active navigation, walking to waypoints | Map reading, planning routes |
| **Cognitive Load** | Low (you always move "forward") | Higher (requires mental rotation) |
| **North Indicator** | Red circle with "N" shows where north is | Not shown (north is always up) |
| **When Stationary** | Maintains last heading for 10 seconds | Always shows north |
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
"I'm walking east toward a waypoint 200m away. On the radar, the waypoint appears directly ahead. When I turn left to go north, the radar rotates and the waypoint now appears to my right. I always know my relative position without thinking."

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

✅ **Stationary Operation**
When standing still and observing waypoint positions - heading-up mode times out after 10 seconds anyway.

**Example Scenario**:
"I have three waypoints forming a triangle north of my position. I want to plan which order to visit them. North-up mode lets me see their absolute positions without the display rotating as I turn around."

---

## How to Switch Modes

### Via Settings Screen

1. Touch the screen or press GPIO0 button to open settings
2. Navigate to **Display** tab
3. Find **Navigation Mode** dropdown
4. Select:
   - **Heading-Up** - Radar rotates with walking direction (default)
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
In heading-up mode, the radar rotates so "up" matches your walking direction. The north indicator lets you know where true north is at any time.

**Example**:
- You're walking **east** (heading = 90°)
- Radar rotates so **east points up**
- North indicator appears on the **left side** of the screen (90° counterclockwise from up)

### North-Up Mode

- **No north indicator** (not needed - north is always up)
- Radar orientation is fixed
- You (red triangle) rotate as you turn

---

## How It Works

### GPS Heading Data

The GPS module (BH-880) provides heading information via UBX NAV-PVT messages:

- **RMC Sentence** (Recommended Minimum Coordinates) includes:
  - **Course**: Direction of movement (0-360°, true north)
  - **Speed**: Speed over ground (knots)
- **Heading is reliable only when moving** (speed > 0.5 knots / ~1 km/h)
- **Stationary GPS heading is invalid** (gyro drift, no motion reference)

**Why Movement Matters**:
GPS determines heading by comparing your current position to previous positions. When stationary, there's no position change, so heading becomes unreliable (can drift randomly).

---

### Coordinate Rotation Algorithm

In heading-up mode, the radar performs **2D coordinate rotation**:

1. Calculate waypoint position relative to you (using Haversine distance)
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

### Stationary Mode Fallback

**Behavior When You Stop Moving**:

1. **First 10 seconds**: Radar maintains your last valid heading
   - Prevents erratic rotation from unreliable stationary GPS data
   - Useful when pausing briefly during navigation
2. **After 10 seconds**: Radar reverts to north-up orientation
   - Assumes you're no longer actively navigating
   - Provides stable reference while stationary

**Example**:
- You're walking east (heading = 90°) → Radar shows east pointing up
- You stop to check map → Radar keeps east pointing up for 10 seconds
- After 10 seconds → Radar rotates back to north-up (heading = 0°)
- You start walking again → Radar immediately updates to new heading

**Why 10 Seconds?**
Balance between:
- Too short (1-2 sec): Annoying rotation when pausing briefly
- Too long (30+ sec): Confusing when stationary and turning around

---

## Performance

### Computational Cost

- **Per-waypoint overhead**: ~35 floating-point operations (Haversine + rotation)
- **50 waypoints**: ~1,750 operations per frame
- **Frame time**: <1 ms @ 240 MHz (negligible)
- **Display refresh**: 60 FPS maintained

### Memory Usage

- **Flash**: +1,848 bytes for entire navigation mode system
- **RAM**: 16 bytes (heading state: `current_heading`, `last_valid_heading`, `last_heading_update`, `heading_up_mode`)
- **No heap allocation** during rotation (stack-based calculations)

### Battery Impact

- **Negligible**: Rotation is pure calculation (no I/O, no radio)
- **Same GPS power**: Heading data comes from existing RMC sentence parsing
- **No additional sensors**: Uses GPS only (IMU fusion not yet implemented)

---

## Troubleshooting

### "Why does the radar point north when I stop?"

**Expected behavior**. GPS heading is unreliable when stationary, so the radar reverts to north-up mode after 10 seconds to provide a stable reference.

**Solution**: Start moving again (>0.5 knots / ~1 km/h) and the radar will resume heading-up mode.

---

### "North indicator not showing"

**Check navigation mode setting**:
1. Open Settings > Display
2. Verify **Navigation Mode** is set to **Heading-Up**
3. North indicator only appears in heading-up mode (not needed in north-up)

**If heading-up is selected but indicator still missing**:
- GPS may not have valid heading data (speed < 0.5 knots)
- Start walking and indicator should appear within 1-2 seconds

---

### "Radar rotates erratically"

**Causes**:

1. **Walking too slowly** (< 0.5 knots / ~1 km/h)
   - GPS heading becomes unreliable at very low speeds
   - **Solution**: Walk at normal pace (3-5 km/h)

2. **Poor GPS signal**
   - Weak signal causes position jitter → erratic heading
   - **Solution**: Move to area with clear sky view, wait for satellite lock

3. **Rapid direction changes**
   - GPS heading updates at 1 Hz (once per second)
   - Very sharp turns may cause brief lag
   - **Solution**: This is normal GPS limitation (consider IMU fusion future upgrade)

---

### "I want faster heading updates"

**Current limitation**: GPS heading updates at 1 Hz (once per second).

**Future enhancement**: IMU sensor fusion (planned):
- QMI8658 gyroscope provides 60+ Hz rotation data
- Smooths heading between GPS updates
- Instant response to direction changes
- See ROADMAP.md for implementation timeline

---

### "Can I change the 10-second timeout?"

**Not currently user-configurable**. The 10-second stationary timeout is hardcoded for optimal balance.

**Developer note**: Timeout is defined in `src/ui/navigation.cpp:537`:
```cpp
if (millis() - ui.last_heading_update > 10000) {  // 10 seconds
    ui.current_heading = 0.0f;  // Revert to north-up
}
```

Modify this value if needed for your application (e.g., 5000 for 5 seconds).

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

### GPS Heading Parsing

**Source**: NMEA RMC sentence (Recommended Minimum Coordinates)

**Fields Used**:
- **Field 7**: Speed over ground (knots)
- **Field 8**: Course over ground (degrees, 0-360° true north)

**Code Location**: `src/hardware/sensors/gps_bh880.cpp:94-143`

**Parsing Logic**:
```cpp
// Speed and Course
if (f_speed && *f_speed) {
    g.speed = atof(f_speed);  // Speed in knots
}
if (f_course && *f_course) {
    g.course = atof(f_course);  // Course in degrees (0-360, true north)
}

// hasHeading is true only if we have valid speed and course, and speed > 0.5 knots
g.hasHeading = (f_speed && *f_speed && f_course && *f_course && g.speed > 0.5);
```

**Data Structure**: `include/hardware/sensors/gps_bh880.h:12-15`
```cpp
struct GPSData {
    // ...
    float  course = NAN;    // Track made good (degrees true north, 0-360°)
    float  speed = NAN;     // Speed over ground (knots)
    bool   hasHeading = false; // True if course is valid (requires movement > 0.5 knots)
    // ...
};
```

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

**Applied In**: `src/ui/navigation.cpp:158-161` (inside `latLonToScreen()`)

---

### North Indicator Rendering

**Code Location**: `src/ui/navigation.cpp:248-286`

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

**NVS Persistence**: `include/settings_manager.h:37`
```cpp
struct RadarSettings {
    // ...
    bool heading_up_mode = true;  // Default: heading-up (user preference)
    // ...
};
```

**Settings UI**: `src/ui/settings_screen.cpp:1012-1051`
- Dropdown with options: "Heading-Up" / "North-Up"
- Immediate effect (no restart required)
- Saves to NVS on change

**Startup Loading**: `src/ui/ui_manager.cpp:54-59`
```cpp
settings_manager::RadarSettings settings;
settings_manager::loadSettings(settings);
g_ui_state.heading_up_mode = settings.heading_up_mode;  // Apply from NVS
```

---

### Update Logic

**Code Location**: `src/ui/navigation.cpp:528-541` (inside `updateRadarDisplay()`)

**State Machine**:
```cpp
if (dev.last_gps_data.hasHeading && dev.last_gps_data.speed > 0.5f) {
    // State: MOVING - update heading from GPS
    ui.current_heading = dev.last_gps_data.course;
    ui.last_valid_heading = dev.last_gps_data.course;
    ui.last_heading_update = millis();
} else {
    // State: STATIONARY
    if (millis() - ui.last_heading_update > 10000) {
        // Substate: TIMEOUT - revert to north-up
        ui.current_heading = 0.0f;
    } else {
        // Substate: HOLD - maintain last heading
        ui.current_heading = ui.last_valid_heading;
    }
}
```

---

## Future Enhancements

### IMU Sensor Fusion (Planned)

**Hardware**: QMI8658 6-axis IMU (already on board)

**Benefits**:
- **60+ Hz rotation updates** (vs 1 Hz GPS)
- **Instant response** to direction changes
- **Works when stationary** (gyroscope detects rotation even without movement)
- **Smooth animation** between GPS heading updates

**Implementation Approach**:
1. Use GPS heading as absolute reference (corrects gyro drift)
2. Use IMU gyro for high-frequency updates between GPS fixes
3. Complementary filter: `heading = 0.98 * (heading + gyro_delta) + 0.02 * gps_heading`

**Challenges**:
- Gyroscope drift accumulation (requires periodic GPS correction)
- Magnetometer needed for absolute heading when stationary (QMI8658 has no magnetometer)
- Sensor fusion complexity (Kalman filter or complementary filter)

**Timeline**: See ROADMAP.md for prioritization

---

### Waypoint-Up Mode (Advanced)

**Concept**: Radar rotates to always show next waypoint "ahead"

**Use Case**: Single-waypoint navigation (hiking to one destination)

**Implementation**: Similar to heading-up, but rotation based on bearing to waypoint instead of GPS heading

**Status**: Not planned (heading-up mode covers most use cases)

---

## Reference Documentation

- **Architecture Details**: See `CLAUDE.md` navigation modes section
- **Implementation History**: See `CHANGELOG.md` v0.10.0 entry
- **Code References**: All locations listed in Technical Implementation section above

---

**Last Updated**: 2025-10-20
**Version**: v0.10.0
**Author**: GPS Radar Development Team
