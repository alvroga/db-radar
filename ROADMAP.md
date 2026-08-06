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

### Quests *(brainstorm stage — not designed yet)*
**Severity**: Feature — new idea, not yet scoped

**Ask**: a GPX file containing multiple waypoints treated as a single "quest" — find all the
waypoints in the set to complete it. Idea originated from the single-file-multiple-waypoints GPX
cache display work (2026-08-05).

**Open questions to resolve before design**:
- How is a "quest" GPX distinguished from a regular multi-waypoint GPX — a naming convention, a tag
  in the file, or explicit user grouping in the web manager?
- How does a waypoint get marked "found" — reuse the existing `Waypoint::found` mechanism (currently
  driven by the 15m sonar-silencing arrival radius, see Sonar Rhythm Defects above) or a separate
  quest-specific state?
- What happens on completion — a reward/notification of some kind ("get something") is the open
  question; no mechanism decided.
- Where does quest progress persist — NVS, alongside the waypoint data, or derived on the fly from
  `found` flags at render time?

**Status**: nothing implemented or designed. Next step is a brainstorming session to scope this
before any code or ADR.

---

### CRT / 8-bit Display Theme *(low priority)*
**Severity**: Cosmetic — user preference, no functional impact

**Ask**: the 480×480 IPS panel renders very sharp/clean; add a toggleable visual style that leans
retro — scanlines, a coarser/pixelated font or dithering, maybe a subtle phosphor-glow color grade —
to evoke a CRT / 8-bit look. Should be a **setting**, not a replacement of the current look (default
stays sharp).

**2026-08-06: scanline/per-pixel route built, tested on hardware, and reverted — rejected on
brightness, not performance.** A halve-brightness darken on alternate output rows was added inline to
`rotate90_tiled`'s existing scatter pass (behind a `rot scanline on|off` serial toggle, no persisted
state, no UI), on the theory examined below that it could ride the pass "for free." Measured on real
hardware: `tiled rotate` 38.9ms → 42.1ms, frame 80.2ms → 86.2ms (+6ms, ~7.5%) — a real, non-zero cost
(see ADR-0026 for why: half the destination rows lose the bulk `memcpy` and fall back to a scalar
per-pixel store loop), but small enough that it wasn't the blocker. **Killed instead by ~20% perceived
brightness loss** on a display already run near its readability floor outdoors — unacceptable
regardless of render cost. Code fully reverted (`git diff` clean against the pre-test commit); nothing
of this experiment ships. Full writeup: [ADR-0026](docs/adr/0026-crt-scanline-brightness-rejected.md).
- Where do scanlines/dither come from — a static overlay image blended over the framebuffer, or a
  per-pixel effect in the flush callback (`rotate90_tiled` in `device_manager.cpp` already touches
  every pixel each frame, so a color/scanline pass could ride along, but that's the exact hot path
  the Render Pipeline section says not to add work to lightly) — **answered above: per-pixel is
  feasible and its cost is real but small; it's dead anyway on the brightness finding.**
- Cost: any per-pixel work in the flush path competes with the frame budget documented in "Render
  Pipeline" above (currently ~85ms/frame, bus-bound) — **measured 2026-08-06, see above: +6ms/frame.**
- ~~Simpler alternative: LVGL theme/font swap only~~ — **chosen 2026-08-04**, scanline/per-pixel route
  rejected (font-only). **Reaffirmed 2026-08-06** after the scanline route was actually built and
  killed on brightness — font-only is now the *only* live route, not just the first choice. Still not
  yet built.

**2026-08-04 scoping pass (font-only route), not yet built**: LVGL ships a built-in pixel/bitmap font
(`LV_FONT_UNSCII_8`/`LV_FONT_UNSCII_16`) currently disabled in `include/ui/lv_conf.h` — enabling it is
one line and near-zero flash/RAM cost, no asset conversion needed. The actual work is bigger than that
one line: fonts are set directly via `lv_obj_set_style_text_font()` at **123 call sites across 7 screen
files** (`ui_manager.cpp`, `waypoint_screen.cpp`, `navigation.cpp`, `dev_screen.cpp`,
`settings_screen.cpp`, `tilt_bench_screen.cpp`, `field_log_screen.cpp`), and UNSCII's fixed small pixel
size differs from Iosevka's 14/16/20px sizing, so a runtime toggle risks shifting label spacing/layout
across every screen — needs on-device visual verification per screen, not just a build check. Not
started; picking this up should budget for that breadth, not treat it as a one-line settings toggle.

**Key files** (font-only is the only live route — see above): `src/ui/settings_screen.cpp` (toggle),
`include/ui/lv_conf.h` (`LV_FONT_UNSCII_8/16`), the 7 screen files above (font call sites)

---

### Waypoint Memory Optimization — cap raised 50 → 500, then rolled back to 200 after a boot failure ✅ resolved at 200
**Severity**: Medium — real GPX files get silently truncated above the cap

**Status (2026-08-05)**: `MAX_WAYPOINTS` is **200**, not the 500 ADR-0022 decided on. 500 shipped,
built clean, and passed a static-SRAM budget check (60.4%, under the documented 80% caution line) —
but that budget was never checked against actual free heap at boot, and the very next boot attempt
failed: `xTaskCreatePinnedToCore` for the Network/System tasks returned `pdFAIL` because only 83,899 B
of internal SRAM remained free before BLE init (down from an estimated ~149KB at cap=50) — BLE
(~25KB) plus the UI (16KB) + I2C (8KB) task stacks it grants first left too little for the two after.
This is exactly the gap ADR-0022 flagged and didn't close: see "Field verification still open" there.
Dropping to 200 (154,656 B / 47.2% static SRAM, a row already computed in ADR-0022's table) fixed it —
**field-confirmed booting on hardware 2026-08-05**, BLE active. The per-waypoint render fix from the
same change (Haversine → equirectangular approximation in `drawWaypoints()`/`latLonToScreen()`,
`src/ui/navigation.cpp`) is unaffected and stays in place — it cut 10 double transcendental
calls/waypoint to 2 multiplies + 1 `sqrtf` (+ conditionally 1 `atan2f`) and is still field-verified
with no regression. `updateWaypointCountLabel()`'s proportional color thresholds
(`src/ui/settings_screen.cpp`) also stay, now proportional to 200 instead of 500.

**Why 200 and not some other number close to the true ceiling**: the failing boot had only 15 real
waypoints loaded (well under any cap), and the array is fixed-size regardless of fill count, so the
200 vs. 500 SRAM difference — not GPX file size — is what the confirmed boot validates. 200 was picked
because it was already a row in ADR-0022's table with a comfortable margin, not because it was
incrementally tuned down from 500. The true ceiling is somewhere in the untested 200–500 range (300
is the next candidate the table already covers, 51.6%/59.4% static SRAM) — raising it further would
need its own hardware boot verification with BLE active before trusting it, the same way this rollback
did.

**Resolved differently than either option above (2026-08-05)**: rather than spending more SRAM on a
higher cap, `MAX_WAYPOINTS` stays 200 and a two-tier index was added instead — every waypoint across
every GPX file gets a lightweight PSRAM-backed index entry (`gpx_index`, up to 8192), and the 200-slot
SRAM array becomes a working set of the closest-200 to the user's actual position, reselected as they
move. This makes "getting past 200" a non-issue for real-world waypoint counts without raising the
SRAM ceiling at all — see [ADR-0023](docs/adr/0023-two-tier-waypoint-index.md) and
`docs/waypoint_two_tier_index_plan.md`. **Field-verified on hardware** against an independent Haversine
oracle (cold selection, a forced high-churn reselect, HDOP gating, a live end-to-end nearby-waypoints
test) — caught and fixed a real bug (`gpx_loader::init()` was never called, so the feature was
silently dead) along the way. Still open: the automatic GPS-driven trigger firing from real movement,
and concurrent SD access during a reselect. A live `memory stats` reading and `wpt_us` at a real
200-waypoint load are also still unmeasured. Decision record for the original 500 call, including why
500 over 700:
[ADR-0022](docs/adr/0022-waypoint-cap-raised-to-500-not-700.md) (superseded on the cap number, not on
the render-cost reasoning).

**Key files**: `include/ui/ui_manager.h` (`MAX_WAYPOINTS`), `src/ui/navigation.cpp`
(`drawWaypoints()`, `latLonToScreen()`), `src/ui/settings_screen.cpp` (`updateWaypointCountLabel()`),
`src/gpx/gpx_index.cpp`/`gpx_loader.cpp` (two-tier index and reselect, ADR-0023)

**Related**: [`docs/performance_optimization_backlog.md`](docs/performance_optimization_backlog.md) step 11,
[`docs/waypoint_cap_increase_investigation.md`](docs/waypoint_cap_increase_investigation.md) (historical — modeled 500, see ADR-0022 for the actual decision),
[`docs/waypoint_two_tier_index_plan.md`](docs/waypoint_two_tier_index_plan.md) / [ADR-0023](docs/adr/0023-two-tier-waypoint-index.md) (closest-N selection instead of a higher cap)

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

### FT-08: Dev Mode Off Doesn't Fully Stop `field_log` — Resolved (2026-08-06)
**Was**: `field_log` (the PSRAM-ring field-data logger used for hardware bring-up/tuning) is started
once at boot if `dev_mode` was on then (`main.cpp:99`), allocating a PSRAM ring buffer and a dedicated
writer task. There was no `field_log::end()`/`stop()` anywhere — only `begin()` and per-session
`startSample()`/`stopSample()` — so turning dev mode off mid-session (even via the otherwise-complete
serial `dev off` cascade, which correctly stops `system_logger`) left the field_log writer task and
ring buffer running for the rest of the session regardless.

**Resolution**: added `field_log::end()` — stops any in-progress sample via the existing stop path,
signals the writer task to exit at a safe point (state `IDLE`, no file open) and blocks briefly until
it self-deletes, then frees the PSRAM ring and control mutex and clears the "begun" flag so a later
`begin()` can restart logging cleanly. Wired into `handleDevCommand`'s `"off"` branch
(`diagnostics.cpp`) alongside the existing `system_logger` disable calls — dev mode has no other
toggle path in the codebase, so this closes the gap completely, not just for the serial command.
Build-verified (RAM 49.3%/161,584 B, Flash 40.5% — unchanged from the ADR-0024 baseline) and
**field-verified on hardware 2026-08-06**: `dev off` printed `[FLOG] Stopped, writer task and PSRAM
ring freed` before the `[DEV] Dev mode OFF` line, and a subsequent `flog start flat360` returned
`[FLOG] begin() not called` — confirming the module was actually torn down (task deleted, ring freed,
`g_begun` cleared), not just superficially disabled.

**Key files**: `src/utils/field_log.cpp`, `include/utils/field_log.h`, `src/utils/diagnostics.cpp`

---

### GPX Storage: Move from SD to FFat — Resolved (2026-08-06)
**Was**: GPX files lived at `/sdcard/gpx`, on a physical SD card the enclosure design makes
inaccessible without disassembly — a bad failure mode for core app functionality. FFat (the flash
partition grown in the same day's OTA resize, see ADR-0024) was completely unmounted.

**Resolution**: `device_manager::initFFat()` mounts the `ffat` partition (wear-levelled FAT,
format-on-first-mount) at `/ffat`; `gpx_loader`/`gpx_server` now read/write `/ffat/gpx` instead of
`/sdcard/gpx`. A one-time boot-time migration copies any pre-existing files from the old SD location
into FFat if the new folder is empty, so upgrading doesn't orphan already-uploaded waypoints. The web
GPX manager gained a storage gauge (`/storage` endpoint, `esp_vfs_fat_info()`) showing FFat usage as a
bar + percentage, placed above the drop area so capacity is the first thing shown. Dev-only logging
stays on SD as decided in ADR-0024 — its web page (`/logs` and related endpoints) is now gated behind
`dev_mode`, returning 404 when off, with the nav link hidden client-side too; its info text now
explicitly calls out that logs live on SD while GPX lives on flash, and its whole look now matches the
GPX page's dark monospace theme instead of its old light purple-gradient one. Both pages also gained a
shared "Select all" + "Download Selected"/"Delete Selected" bulk UI (multi-download is staggered
`<a download>` clicks, one per file — no on-device zip capability). Build-verified: Flash 40.5%
(+19.9 KB over the ADR-0024 baseline), RAM 49.3% (+80 B). **Field-verified 2026-08-06**: first-boot
FAT formatting and the SD→FFat migration copy both confirmed working — waypoints uploaded before the
migration survive and load correctly from `/ffat/gpx`. The dev-mode-gated logs page and the bulk
select/download UI haven't specifically been exercised on hardware yet.

**Full reasoning**: [ADR-0024](docs/adr/0024-ota-partitions-grown-from-unused-ffat.md)

---

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
