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
- ~~Simpler alternative: LVGL theme/font swap only~~ — **chosen 2026-08-04**, scanline/per-pixel route
  rejected. Font-only stays open as a *scope*, not a decision, until it's actually built.

**2026-08-04 scoping pass (font-only route), not yet built**: LVGL ships a built-in pixel/bitmap font
(`LV_FONT_UNSCII_8`/`LV_FONT_UNSCII_16`) currently disabled in `include/ui/lv_conf.h` — enabling it is
one line and near-zero flash/RAM cost, no asset conversion needed. The actual work is bigger than that
one line: fonts are set directly via `lv_obj_set_style_text_font()` at **123 call sites across 7 screen
files** (`ui_manager.cpp`, `waypoint_screen.cpp`, `navigation.cpp`, `dev_screen.cpp`,
`settings_screen.cpp`, `tilt_bench_screen.cpp`, `field_log_screen.cpp`), and UNSCII's fixed small pixel
size differs from Iosevka's 14/16/20px sizing, so a runtime toggle risks shifting label spacing/layout
across every screen — needs on-device visual verification per screen, not just a build check. Not
started; picking this up should budget for that breadth, not treat it as a one-line settings toggle.

**Key files**: `src/core/device_manager.cpp` (flush callback, if a per-pixel effect is chosen),
`src/ui/settings_screen.cpp` (toggle), `include/ui/lv_conf.h` (`LV_FONT_UNSCII_8/16`), the 7 screen
files above (font call sites) if font-only

---

### Waypoint Memory Optimization — raise the 50-waypoint cap
**Severity**: Medium — real GPX files get silently truncated

**Symptom**: `RadarConfig::MAX_WAYPOINTS = 50` is a hard *load-time* cap. `gpx_loader.cpp:376` stops parsing at 50 and sets `was_truncated`. A geocaching.com pocket query routinely contains hundreds of caches, so most of the file never loads.

**The SRAM ceiling that motivated this cap is gone** — see Resolved below: `desc`/`hint` moved to PSRAM, freeing ~64 KB. `MAX_WAYPOINTS` itself has **not** been raised yet; this entry is now just that remaining step.

**Note on the render side**: the cap is *not* what limits drawing. `drawWaypoints()` culls by distance before drawing (`navigation.cpp:633`) and renders only the fixed waypoint when one is selected (`navigation.cpp:614`), so draw cost scales with *visible* waypoints. What does scale with the cap is the per-waypoint Haversine loop — and the ESP32-S3 FPU is single-precision only, so all that `double` trig is soft-float. Read `wpt_us` off the `perf` HUD before raising the cap; §3.6 of the perf backlog proposes an equirectangular approximation that would cut it.

**2026-08-04 investigation complete, no code changed yet**: SRAM math confirmed clean (`sizeof(Waypoint)`
measured at 144 B, not assumed — raising the cap to 500 costs +63.3 KB SRAM / +562.5 KB PSRAM, landing
static RAM at ~60.4%, close to a level this project already ran on safely pre-PSRAM-migration) and every
other `MAX_WAYPOINTS` call site audited (no hidden LVGL/stack scaling risk — one real bug found:
`updateWaypointCountLabel()`'s color thresholds are hardcoded to a cap of 50 and need to scale with it).
The per-waypoint Haversine loop (10 double transcendental calls/waypoint, unconditional, before the
distance filter) is flagged as the one real unverified risk — recommendation is to land the §3.6
equirectangular rewrite first and field-verify `wpt_us` at a real high waypoint count before picking a
final number. Full analysis: [`docs/waypoint_cap_increase_investigation.md`](docs/waypoint_cap_increase_investigation.md).

**Key files**: `include/ui/ui_manager.h` (`Waypoint`, `MAX_WAYPOINTS`), `src/gpx/gpx_loader.cpp`

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

### Compass Calibration & Tilt — heading is only valid held flat
**Severity**: High — the compass is the sole heading source, and it inverts when the device is tilted
**Status**: WP-0 through WP-5 done. Field trip (WP-2) and offline analysis (WP-3) complete 2026-08-01/02
— **go decision for Level 3 tilt compensation**. WP-4 (Level 1 health metrics: `H0`/residual/axis-ratio
in the calibration overlay, runtime `classifyHealth()`, HUD trust indicator) shipped 2026-08-02. WP-5
(Level 2: 3-axis calibration — a second, tumble/figure-8 calibration step that recovers a real
`cal_z_offset` via 3-D coverage scoring) shipped 2026-08-02 — see CHANGELOG.md and ADR-0019. WP-6
(accelerometer tilt compensation) **shipped 2026-08-02** — see CHANGELOG.md and
[ADR-0020](docs/adr/0020-tilt-compensation-formula-and-sign-from-bench-data.md). The mag↔accel frame
rotation was measured via the Settings > DEV > Tilt Bench protocol
([`docs/compass_tilt_bench.md`](docs/compass_tilt_bench.md)), a vector cross-product formula (not the
textbook roll/pitch one, which fit poorly) and a bench-derived sign were implemented in
`navigation/tilt_compensation`, and both were confirmed in live hand-held use. The one open item is
cosmetic, not a defect: a fast flat→nose-up tilt shows a ~30° transient heading bounce that recovers
within ~1s, caused by the gravity EMA lagging the tilt change mid-motion — expected for an accel-only
(no gyro, [ADR-0018](docs/adr/0018-tilt-compensation-required-gyro-deferred.md)) approach, and already
tightened once (τ 1.0s → 0.5s) in response.

**Symptom** (field-confirmed): facing north held **flat**, N points to the top — correct. Facing north
held **vertical**, N points to the **bottom**. The heading formula is 2-axis (`atan2(cy, cx)`), so the
large vertical field component leaks in as tilt grows; the crossover is at only **90° − inclination
≈ 31.5°**, past which the reading inverts. Phone-style holding is deep past it. Separately, nothing
detects a stale calibration — opening the enclosure invalidates it silently.

**WP-3 confirmed the tilt error is heading-dependent, not a fixed bias** — at ~46–50° tilt, heading
error vs GPS course was −135° walking north and +4° walking south. No lookup-table correction can fix
that; only real accelerometer-based tilt compensation can. Flat holding stays accurate in every
direction tested (under 8° mean error). Also answered: `H₀` ≈ 3000 (raw units), a figure-8 tumble
covers the full sphere (3-D calibration is feasible), and accel-only (no gyro) suffices for tilt
compensation provided it's oversampled/averaged rather than read at a flat 10 Hz. Full numbers:
[`docs/calibration/wp3_results.md`](docs/calibration/wp3_results.md).

**Plan**: four staged levels — (1) magnitude-based health metrics ✅ done, free and no new hardware;
(2) 3-axis calibration ✅ done — a second tumble/figure-8 step, min/max-per-axis rather than an
ellipsoid fit (ADR-0019); (3) accelerometer tilt compensation — next; (4) τ-per-zoom smoothing. Gyro is
rejected for tilt fusion — WP-3's shake-spectrum analysis found no need for it (ADR-0018).

**Resolved along the way**: `HEADING_SMOOTHING` went 0.8@1Hz → 0.3@10Hz, which did **not** preserve
the time constant (0.62 s → 0.28 s). Restored to α = 0.15 (τ ≈ 0.62 s) and **verified on a walk** —
the heading bounce is gone. See CHANGELOG.md.

**Supersedes** FT-02 below, whose "won't fix" reasoning assumed a 1 Hz compass rate.

**Full design, field protocol and implementation plan**:
[`docs/compass_calibration_foundation.md`](docs/compass_calibration_foundation.md)

---

## Resolved

### FT-06 / FT-07: I2C Bus Freeze / UI Freeze Regression — Resolved (2026-08-05)
**Was**: a recurring full or partial interface freeze (button, touch, display all unresponsive),
reported across four occurrences 2026-07-31 to 2026-08-02, needing a manual power cycle at least once.
FT-07 was the same bug recurring on Settings > DEV with zero user interaction.

**Root cause**: a real ESP-IDF driver bug, not application logic — `i2c_master.c`'s NACK-handling
busy-wait had no timeout bound at all (`components/esp_driver_i2c`, pinned IDF 5.5.0), so any I2C call
that NACKed without the hardware autonomously completing the STOP condition spun the calling task
forever, bypassing the application's own timeout entirely. Matches Espressif's own upstream issue
[#17720](https://github.com/espressif/esp-idf/issues/17720), fixed in IDF v5.5.4 — three point releases
past what PlatformIO's registry offers for the 5.5.x line, with the next available version a
major-version jump (6.0.x) out of scope for one driver bug.

**Resolution**: `scripts/patch_i2c_master_nack_hang.py` backports just the upstream timeout-bound fix
into the vendored driver at build time (idempotent, portable via `PioPlatform().get_package_dir()`).
`panic_on_timeout` (TWDT) stays on as a safety net for any other hang mechanism.

**Field verification (2026-08-05)**: two consecutive sessions, ~10.5 hours of combined logged active
runtime (~8h51m + ~1h48m), spanning an overnight standby/wake cycle and an unplanned power-loss
recovery (the device was dropped mid-session). Zero freezes, zero `TASK_WDT`/`PANIC` reset reasons,
unbroken 60s heartbeat cadence throughout both sessions (the freeze's signature is the heartbeat
silently stopping — never observed), I2C failure rate ~0.01% in both, never enough to trip the
consecutive-failure recovery watchdog ([ADR-0003](docs/adr/0003-proactive-i2c-bus-recovery-watchdog.md)).

**Kept open, not blocking**: *why* this bus produces NACKs at all (signal integrity, bus voltage,
clock-stretching, EXIO register corruption) is still unconfirmed — the patch bounds the hang regardless
of the NACK's cause.

**Full analysis**: [`docs/i2c_bus_freeze_investigation.md`](docs/i2c_bus_freeze_investigation.md) ·
[ADR-0021](docs/adr/0021-i2c-nack-hang-build-time-backport.md)

---

### GPX Manager Name Display + Logs Bulk Delete — Resolved (2026-08-04)
**Was**: uploaded GPX filenames on the web upload page are bare geocache codes (e.g. `GC38EVJ.gpx`),
giving no clue which cache is which without opening the file. Separately, the `/logs` page could only
delete one log file at a time — tedious when clearing a card full of field-session logs.

**Resolution**: `/list` now returns each file's `<groundspeak:name>` (or the waypoint's own `<name>`
GC code as a fallback) alongside the filename; the upload page shows it as the primary label with the
code underneath. `/logs` gained a "Select all" checkbox and a "Delete Selected" button, built on the
existing per-file delete endpoint rather than a new bulk one. Built while FT-06 field-verification is
pending — no hardware dependency, `pio run` build passes clean (+672 B RAM). Not yet browser-tested on
device.

**Full detail**: [CHANGELOG.md](CHANGELOG.md) → Unreleased → Added

---

### Waypoint desc/hint moved to PSRAM — Resolved (2026-07-31)
**Was**: `g_ui_state` was the largest firmware symbol (70,992 B, ~37% of static RAM), almost entirely `desc[1024]`/`hint[256]` × 50 waypoints, read in exactly one place (the detail screen, one waypoint at a time) but resident for all 50 permanently.

**Resolution**: moved into a `WaypointDetail` block allocated once in PSRAM (`heap_caps_calloc(..., MALLOC_CAP_SPIRAM)` in `ui_manager::init()`); `Waypoint::desc`/`hint` are now pointers into it, guarded against a failed allocation rather than crashing. Frees ~64 KB SRAM — and flash dropped by almost the same amount too, confirmed via a `readelf` section diff (see CHANGELOG for why). **Verified on hardware, no regressions.**

**Does not by itself raise `MAX_WAYPOINTS`** (still 50) — see Planned above for that follow-up.

**Full analysis**: [`CHANGELOG.md`](CHANGELOG.md) · [ADR-0001](docs/adr/0001-waypoint-detail-psram-cache.md)

---

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
