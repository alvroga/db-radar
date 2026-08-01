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

### CRT / 8-bit Display Theme *(low priority)*
**Severity**: Cosmetic — user preference, no functional impact

**Ask**: the 480×480 IPS panel renders very sharp/clean; add a toggleable visual style that leans
retro — scanlines, a coarser/pixelated font or dithering, maybe a subtle phosphor-glow color grade —
to evoke a CRT / 8-bit look. Should be a **setting**, not a replacement of the current look (default
stays sharp).

**Open questions to resolve before implementation**:
- Where do scanlines/dither come from — a static overlay image blended over the framebuffer, or a
  per-pixel effect in the flush callback (`rotate90_tiled` in `device_manager.cpp` already touches
  every pixel each frame, so a color/scanline pass could ride along, but that's the exact hot path
  the Render Pipeline section says not to add work to lightly)
- Cost: any per-pixel work in the flush path competes with the frame budget documented in "Render
  Pipeline" above (currently ~85ms/frame, bus-bound) — needs a real measurement, not an assumption
- Simpler alternative: LVGL theme/font swap only (pixel font, no scanlines) — near-zero render cost,
  weaker effect

**Key files**: `src/core/device_manager.cpp` (flush callback, if a per-pixel effect is chosen),
`src/ui/settings_screen.cpp` (toggle), LVGL theme/font config if font-only

---

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

### Beacon Direction Finding — "which way do I start walking?"
**Severity**: Feature — the highest-value thing this hardware could still learn to do
**Status**: unblocked 2026-07-31, not yet built

**Can we do it?** True Bluetooth 5.1 AoA is **impossible** here — single antenna, no CTE IQ samples. But **body-shadow DF** works: the body attenuates 2.4 GHz by 10–20 dB, so rotating in place produces a directional RSSI signature, and the QMC5883L supplies the heading for every sample. Bearing comes from a first-circular-harmonic fit with a resultant-length confidence gate, not from `argmax`.

**Was blocked, is no longer blocked**: at 2 Hz BLE a 10-second rotation yielded **1.7 samples per 30° bin** — pure noise. The prerequisite rate work (below, in Resolved) is done and **measured live at 4.24–4.37 Hz**, giving ~3.6 samples/bin — workable outdoors, per the sample-rate table in `docs/beacon_direction_finding.md` §3. Reconfiguring the tag to 100 ms would get to 10 Hz / 8.3 samples/bin (not yet done — the tag is currently at 200 ms).

**Expected**: ±30–45° outdoors at 10–40 m — reliable quadrant, i.e. "start walking that way". Unreliable indoors (multipath); the confidence gate must refuse rather than guess.

**Must be measured, not derived**: the sign of the peak. Body shadowing says peak = beacon direction, but the device's own asymmetric pattern may offset or invert it. Calibrate against a known bearing before trusting it.

**Complement**: GPS gradient DF — log `(lat, lon, rssi)` while walking and trilaterate over two ~15 m legs. More robust outdoors, needs no user ritual, and could be built first.

**Full design**: [`docs/beacon_direction_finding.md`](docs/beacon_direction_finding.md)

---

## Resolved

### Beacon Responsiveness — the BLE feed was rate-starved at 2 Hz — Resolved (2026-07-31)
**Was**: beacon proximity felt sluggish. ~3.3 s from moving to the ring changing, and the ring only had 4 states.

**Root cause**: not CPU — the 240 MHz change bought this nothing. Three things combined into a hard ceiling of one RSSI sample per 500 ms: NimBLE's default controller-side duplicate filtering, `g_pScan->stop()` on first hit, and `SCAN_INTERVAL_MS = 500`. The tag advertised at 200 ms, so 60–80% of available packets were discarded.

**Resolution**: one continuous passive scan (duplicates off, results not stored, 100 ms window == interval), τ-based EMAs re-derived in *time* rather than samples, zone confirmation as a duration rather than a sample count, and a continuous ring width driven from the already-computed-but-previously-unread `rssi_display`. **Verified live on hardware: 4.24–4.37 Hz** (mean gap ~230 ms, was ~500 ms), `Scan callbacks` climbing at ~89/sec.

**Follow-up bug found in the field**: the first version of the priority/continuous-tempo work drove the sonar *tempo* from the fast EMA (`rssi_ema`, τ=0.5s) instead of the slow one, and switched beep *length* on a 3-state trend enum instead of interpolating — both reported as "choppy" beeping and both fixed same-day (tempo now from `rssi_display` with τ raised 1.0→2.0s; beep length continuous from the raw regression slope).

**Full analysis**: [`docs/performance_optimization_backlog.md`](docs/performance_optimization_backlog.md) §7 · [`docs/beacon_proximity.md`](docs/beacon_proximity.md)

---

### Sonar Rhythm Defects — Resolved (2026-07-31)
**Was**: two independent defects found in the 2026-07-31 audit, both audible.

1. **The sonar grid re-based off actual fire time** (`= now + interval` instead of `+= interval`), giving up to 20 ms jitter per beat (8% at the 250 ms CLOSE interval) and a systematic ~4% flat tempo.
2. **Waypoint sonar had no hysteresis** — hard boundaries at 5/10/30/50 m evaluated at 10 Hz against ±2–5 m of GPS jitter, so standing still near a boundary flipped the tempo between two rates at random.

**Resolution**: the beat-grid fix advances by a fixed step with a catch-up guard. The waypoint sonar was rebuilt entirely as a **continuous geometric mapping** (2000 ms @ 50 m → 250 ms @ 2 m) with a τ=1.5s EMA on distance as the noise guard, which *subsumed* the hysteresis fix rather than sitting alongside it — a continuous tempo has no rates to flicker between. Also added: an **arrival stop** (the sonar now honours `Waypoint::found`, silencing on tap-within-15m exactly like the beacon ball) and **beacon-absolute-priority** (`isInRange()` now releases any fixed waypoint outright rather than merely yielding the buzzer).

**⚠️ One attempted follow-on was tried and reverted**: halving `I2C_PROCESS_MS` (20→10ms) to further reduce the buzzer's timing-quantization floor broke the device on hardware (button unresponsive, buzzer silent) — I2C bus contention with the touch driver, not a CPU cost problem. Reverted and confirmed fixed. `I2C_PROCESS_MS = 20` is now a documented hard floor; see `memory/i2c_process_ms_floor.md`.

**Full analysis**: [`docs/performance_optimization_backlog.md`](docs/performance_optimization_backlog.md) §8.1

---

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
