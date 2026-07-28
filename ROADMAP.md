# CC-Radar Roadmap

**GPS Radar Navigation System for Waveshare ESP32-S3-Touch-LCD-2.1**

For completed features and history, see [CHANGELOG.md](CHANGELOG.md).

---

## Known Issues

### FT-03: Zoom Levels Not Progressive
**Severity**: Medium — navigation confusion

**Symptom**: A waypoint visible at one zoom level can vanish at the next. The current radii (50m, 100m, 500m, 1km, 5km) are not geometric multiples of each other — jumps alternate between 2× and 5×. A waypoint near the outer ring of 100m zoom may fall outside the distance filter threshold for 500m zoom.

**Proposed fix**: Replace with a geometric progression where each step is a fixed multiplier (e.g. ×3 or ×4): 50m → 150m → 500m → 1500m → 5000m. Every waypoint visible at zoom N would be visible at N+1, just further from centre.

**Also affects**: Grid line spacing, off-screen indicator distance filter multiplier.

**Key file**: `include/ui/ui_manager.h` — `RadarConfig::ZOOM_CONFIGS[]`

---

### FT-05: On/Off-Screen Boundary Duplicate Indicator
**Severity**: Low — edge case visual glitch

**Symptom**: As a waypoint crosses from off-screen to on-screen, both the yellow dot and the orange off-screen arrow briefly appear simultaneously.

**Root cause**: Off-screen sector assignment occurs before the on-screen clip check. Waypoints exactly at the boundary pixel satisfy both conditions.

**Fix**: Strict boundary guard in `drawWaypoints()` — only add to off-screen sector if `x < 0 || x >= screen_size || y < 0 || y >= screen_size` (exclusive bounds check). One-line fix.

**Key file**: `src/ui/navigation.cpp` — `drawWaypoints()`

---

## Planned

### Waypoint Memory Optimization — raise the 50-waypoint cap
**Severity**: Medium — real GPX files get silently truncated

**Symptom**: `RadarConfig::MAX_WAYPOINTS = 50` is a hard *load-time* cap. `gpx_loader.cpp:376` stops parsing at 50 and sets `was_truncated`. A geocaching.com pocket query routinely contains hundreds of caches, so most of the file never loads.

**Root cause — it's a RAM ceiling, not a render ceiling.** `g_ui_state` is the single largest symbol in the firmware at **70,992 bytes** (next largest is `work_mem_int$4` at 65,536), which is ~37% of all static RAM. Almost all of it is the waypoint array:

| Field | Bytes |
|---|---|
| `desc[1024]` | 1024 |
| `hint[256]` | 256 |
| `display_name[64]` | 64 |
| `name[48]` | 48 |
| lat/lon/valid/found | 24 |
| **`sizeof(Waypoint)` (padded)** | **~1416** |

× 50 = ~70,800 B. **`desc` + `hint` alone are 90% of it** — and they are read in exactly one place, `waypoint_screen.cpp:117,149`, the detail screen for a *single* waypoint at a time. All 50 copies sit resident in SRAM permanently to serve one on demand.

**Proposed fix**: move `desc`/`hint` out of SRAM — either into PSRAM (8 MB, effectively free) or drop them from RAM entirely and re-read from the GPX file when a waypoint is tapped. `Waypoint` drops to ~136 bytes:

- same 50 waypoints → ~6.8 KB instead of 70.8 KB (**frees ~64 KB SRAM**)
- or the same ~70 KB budget buys **~500 waypoints**

**⚠️ Implementation constraint**: allocate via `ps_malloc()` in `ui_manager::init()`. Do **not** use section attributes — `.ext_ram_noinit` causes a boot crash on this ESP-IDF version because constructors are not called for objects placed there. Keep the hot fields (LVGL pointers, zoom, heading, GPS centre — ~300 bytes) in SRAM and move only the `Waypoint[]` array.

**Note on the render side**: the cap is *not* what limits drawing. `drawWaypoints()` culls by distance before drawing (`navigation.cpp:633`) and renders only the fixed waypoint when one is selected (`navigation.cpp:614`), so draw cost scales with *visible* waypoints. What does scale with the cap is the per-waypoint Haversine loop — and the ESP32-S3 FPU is single-precision only, so all that `double` trig is soft-float. Read `wpt_us` off the `perf` HUD before raising the cap; §3.6 of the perf backlog proposes an equirectangular approximation that would cut it.

**Key files**: `include/ui/ui_manager.h` (`Waypoint`, `MAX_WAYPOINTS`), `src/gpx/gpx_loader.cpp`, `src/ui/waypoint_screen.cpp`

**Related**: [`docs/performance_optimization_backlog.md`](docs/performance_optimization_backlog.md) step 11

---

### Beacon Advertising Interval Note
The beacon proximity system is most responsive when the target beacon advertises at a short interval (~100ms). Longer advertising intervals (500ms+) cause noticeable lag between physical proximity change and RSSI update. Consider documenting the recommended beacon configuration in `docs/beacon_proximity.md`.

---

## Resolved

### FT-01: Button Double-Tap Unresponsive — Resolved (2026-03-20)
**Was**: Double-tap to reverse zoom unreliable, felt unresponsive under load.
**Resolution**: NimBLE migration freed ~40KB SRAM that was exhausted by the Bluedroid BLE stack. Heap stalls that disrupted button state machine timing are gone. Double-press detection now reliable.

---

### FT-04: Beacon Sound Choppy at Close Range — Resolved (2026-03-25)
**Was**: Buzzer pattern stuttered at 50m zoom when near a beacon.
**Resolution**: Beacon sonar reworked to a 4-zone musical tempo system (VERY_FAR 1500ms / FAR 750ms / MEDIUM 500ms / CLOSE 250ms) with EMA smoothing, hysteresis, and trend detection. NimBLE migration also resolved the underlying SRAM pressure that caused I2C contention on EXIO buzzer writes.

---

## Won't Fix

### FT-02: Compass Zoom-Dependent Smoothing
**Was**: At large zoom levels (1km, 5km) compass noise produces visible jitter.
**Decision**: Won't fix. Zoom-dependent EMA is not viable at 1Hz compass rate — heavy smoothing at large zoom would make the radar sluggish and unresponsive to real turns. At walking speeds and practical zoom levels the current 1Hz rate is acceptable. The correct long-term solution is a higher compass update rate (requires moving compass wires to Wire1 on GPIO19/20 for a dedicated I2C bus), not software smoothing.
