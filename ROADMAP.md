# CC-Radar Roadmap

**GPS Radar Navigation System for Waveshare ESP32-S3-Touch-LCD-2.1**

For completed features and history, see [CHANGELOG.md](CHANGELOG.md).

---

## Known Issues

None currently open. See Resolved below for FT-03, FT-05, FT-09.

---

## Planned

### Dual GPS Module Support (BH-880 + LC76G) — built, not yet field-tested
**Severity**: Feature — restores LC76G compatibility (dropped when the project moved to the BH-880)
without needing a separate build

**Status**: implemented on the `feature/dual-gps-module` branch, deliberately **not** merged into
`initial-release` — build-verified clean but not yet tested on real hardware for either module.
`gps_bh880.cpp` now speaks both UBX (BH-880) and NMEA/PAIR (LC76G), auto-identified on first boot and
then pinned as a persisted choice (Settings > GPS) rather than re-detected every boot. A board with no
compass (LC76G-only) is forced to North-Up automatically. See
[CHANGELOG.md](CHANGELOG.md) and [ADR-0032](docs/adr/0032-pinned-gps-module-not-always-auto-detect.md)
for full detail. Merge into `initial-release` once verified on both a BH-880 and an LC76G board.

---

### Quests *(design substantially resolved, feature not yet built)*
**Severity**: Feature — design largely settled, implementation not started

**Ask**: a GPX file containing multiple waypoints treated as a single "quest" — find all the
waypoints in the set to complete it, with progress tracking and a small collectible badge on
completion. Idea originated from the single-file-multiple-waypoints GPX cache display work
(2026-08-05).

**Status**: past the brainstorm stage. The full design — tagging mechanism, data model, tap-to-
confirm behavior (generalized from "fixed waypoint only" to "any in-range waypoint," a prerequisite
change with its own real code impact), badge format/storage, NVS persistence — is written up and
mostly resolved in [`docs/quests_plan.md`](docs/quests_plan.md). Real prep code has already shipped
ahead of the feature itself: `gpx_index`'s file capacity was raised (64 → 1024) and `file_id` widened
to `uint16_t` specifically to handle "many small quest files" instead of "a few large ones" (see
CHANGELOG.md and `docs/quests_plan.md` §0.5). The quest feature's own tagging/tracking/badge code has
not been built yet. Remaining open items (badge filename scheme, web quest creator UI, milestone
badge thresholds) are listed at the end of `docs/quests_plan.md`.

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

### Event Pixel Art / Image Assets — brainstorm stage, not designed yet
**Severity**: Feature — new idea, not yet scoped

**Ask**: static pixel-art images shown for discrete events (e.g. quest completion, once Quests above
has real state), also usable standalone for other one-off events (GPS lock acquired, a milestone
badge, etc.) without depending on Quests shipping first.

**Budget context (sanity check only, not a decision)**: firmware is ~1.68MB in a 4MB OTA slot (~60%
headroom). `LV_COLOR_DEPTH` is 16 (RGB565) project-wide (`include/ui/lv_conf.h`) — there is no true
32bpp render path here without a global change that would also inflate every framebuffer, so a literal
ARGB8888 asset format isn't the right target. Indexed 4/8-bit palettes cost roughly a third of
RGB565+8-bit-alpha (`LV_IMG_CF_TRUE_COLOR_ALPHA`) for the same dimensions and suit pixel art's
naturally low color count — e.g. a 150×150 sprite is ~23KB indexed vs ~68KB true-color+alpha. A few
hundred KB earmarked for art (dozens of small indexed sprites) leaves most of the current headroom for
code.

**Open questions to resolve before design**:
- Compiled into flash as LVGL image C arrays (simple, spends OTA slot headroom directly) vs. loaded
  from FFat at runtime via `lv_fs` (matches the GPX/web-upload pattern already built — art becomes
  updatable from the web portal without a reflash, at the cost of a filesystem driver + an
  event→sprite lookup)
- Color format/bit depth per image (indexed 4/8-bit vs 16-bit+alpha) — affects flash size and blit cost
- Which events warrant art beyond quest completion, and whether that's decided before or after Quests
  itself is scoped

**Status**: nothing implemented or designed.

---

### Radar Animation Effects from Capsule Radar Research — brainstorm stage, not designed yet

**Severity**: Feature — new idea, not yet scoped

**Ask**: research (a Claude Code fork reading capsule-radar's actual rendering source) into whether
its visual effects — a rotating sweep beam, staggered sonar/glow rings — could port to db-radar's
draw callback. Two candidates look directly implementable with no architecture change; a third
(persistent `lv_canvas` heatmap) is a hard no, since it contradicts the no-canvas render design.
**Status**: research only, nothing designed or implemented. Full findings (implementation approach,
what doesn't port over and why, measurement requirements, open questions):
[`docs/radar_animation_effects_research.md`](docs/radar_animation_effects_research.md).

---

### First-Flash Procedure for a New Board — not yet verified end-to-end
**Severity**: Process — a new board is inbound and needs a defined, low-friction bring-up path

**Ask**: first image onto a blank board via USB + PlatformIO/VSCode (should handle bootloader,
partition table, and app in one step), then every subsequent update via the already-built web
`/update` OTA flow (`gpx_server.cpp` — `esp_ota_begin`/`esp_ota_write`/`esp_ota_set_boot_partition`) —
not USB again.

**Already confirmed by inspecting the build** (`.pio/build/db-radar/flasher_args.json`): `pio run -t
upload` writes all four required regions in one command — bootloader (`0x0`), partition table
(`0x8000`), app to `ota_0` (`0x10000`), **and** `ota_data_initial.bin` to the `otadata` partition
(`0xe000`). A blank chip's otadata is pre-seeded to boot `ota_0` correctly by this alone; no manual
esptool/otadata step should be needed.

**Still open / needs verification on the actual new board**:
- `esp_ota_mark_app_valid_cancel_rollback()` (`main.cpp:421`) must run on first boot or the bootloader
  will roll back on the next reset — `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` is set, so this isn't
  optional. Confirmed present in code; not confirmed firing correctly on a genuinely blank chip (only
  ever exercised on already-provisioned boards so far).
- NVS starts blank on new hardware (unlike FFat, which auto-formats on first mount per the GPX-to-FFat
  entry below) — compass calibration (tumble + figure-8, Settings > Display), WiFi/beacon settings,
  and `dev_mode` state all need to be redone from scratch, not just copied over.
- GPX waypoints: nothing pre-loaded — the SD→FFat auto-migration was removed 2026-08-07 (see
  CHANGELOG.md, [ADR-0027](docs/adr/0027-remove-sd-ffat-gpx-migration.md)); always re-upload via the
  web portal on a fresh device.
- Whether the `/update` OTA path needs anything different for the *first* real OTA on a freshly-USB-
  flashed board vs. steady-state — expected not to, since otadata is already valid after the USB
  flash, but not yet tested end-to-end on new hardware.

**Browser-based first-flash — resolved 2026-08-08**: a tag-triggered release pipeline
(`.github/workflows/release.yml`) now builds via PlatformIO, merges bootloader/partition-table/
otadata/app into one image via `esptool merge_bin`, publishes both that full-flash binary and an
app-only OTA binary to GitHub Releases, and deploys an ESP Web Tools flasher page (`web/flasher/`) to
GitHub Pages — all from the same build, so the Release asset and the web flasher can never disagree.
Hosting is GitHub Pages; "which binary do we hand out" is answered by the release process itself. See
[`docs/firmware_installation.md`](docs/firmware_installation.md),
[ADR-0030](docs/adr/0030-release-pipeline-build-time-pages-not-committed-binaries.md),
[ADR-0031](docs/adr/0031-single-part-manifest-not-multi-part.md).

**Scope narrowed same day**: shipping with only the 2 binary-distribution paths (web flasher +
`esptool.py`/Releases) — the PlatformIO source-build install option is intentionally held back for
now, repo isn't public yet. **Not yet exercised end-to-end on a real tag push, and the repo is
currently private** (both install paths are inert until it's made public — GitHub Pages and public
Release downloads both require a public repo on the free plan) — see
`docs/firmware_installation.md`'s Verification status section for the full remaining checklist.

**Status**: nothing verified on real new hardware yet. Walk this checklist in order when the new board
arrives and turn it into a field-verified procedure.

---

## Resolved

### FT-09: Waypoint On-Screen Test Used a Square Box on a Round Display — Resolved (2026-08-06)
**Was**: reported by the user via two annotated screenshots showing a waypoint sitting inside the
"next zoom's" inner quadrant that should stay visible when zooming in, plus a follow-up screenshot
showing where it actually went: the dot rendered into the black square corner of the framebuffer,
outside the round visible glass. `drawWaypoints()`'s on/off-screen test (`src/ui/navigation.cpp`) was
`x >= 0 && x < screen_size && y >= 0 && y < screen_size` — a square bounding-box check against the
full 480×480 framebuffer, not the round display area. A waypoint landing in the square's corners
(inside the 480×480 bounds but outside the round visible radius) passed this test and was drawn as a
dot hidden under the bezel, instead of falling through to the existing off-screen sector-arrow logic —
so it appeared to simply vanish, most noticeably on the 100m → 50m zoom step.

**Resolution**: replaced the square test with a circular one (`dx²+dy² <= (screen_size/2)²`) in both
`drawWaypoints()` and the tap hit-test in `handleTapAt()` (needed the same fix for hit-test/render
consistency). Build-verified: RAM 49.3% (161,584 B), Flash 40.5% (1,698,859 B) — unchanged.
**Field-verified 2026-08-06** — user confirmed on hardware the 100m → 50m corner case now behaves
correctly.

**Does not fully resolve FT-03** (below) — see that entry for why the two look similar but aren't
the same issue.

**Key file**: `src/ui/navigation.cpp` — `drawWaypoints()`, `handleTapAt()`

---

### FT-03: Zoom Levels Not Progressive — Closed, accepted as-is (2026-08-06)
**Was**: written against an older zoom set (50m, 100m, 500m, 1km, 5km) with jumps alternating 2×/5×,
proposing a strict geometric progression (e.g. ×3 or ×4 every step) so a waypoint visible at zoom N
would always still be visible at N+1.

**Closed without code changes**: the zoom set in `RadarConfig::ZOOM_CONFIGS[]` had already moved on to
50m → 100m → 200m → 500m → 1000m — mostly 2× steps with one 2.5× jump (200m → 500m) — without this
entry being updated to match. Re-tested against the current set: the user reports the zoom now feels
natural with no navigation-confusion symptom. Root cause of the stale entry was the same as FT-05's —
the zoom levels were revised at some point and ROADMAP.md wasn't updated alongside. No further work
planned; the one non-uniform step (200m → 500m) is accepted.

**Key file**: `include/ui/ui_manager.h` — `RadarConfig::ZOOM_CONFIGS[]`

---

### FT-05: On/Off-Screen Boundary Duplicate Indicator — Closed, unreproducible (2026-08-06)
**Was**: reported symptom was a waypoint showing both the yellow on-screen dot and the orange
off-screen arrow simultaneously while crossing the visibility boundary — originally observed walking
toward a waypoint that started out of bounds.

**Closed without a targeted fix**: user deliberately tried to reproduce the original scenario (walking
a waypoint from out-of-bounds into bounds) during the FT-09 investigation above and could not — only
one of the two ever renders. This matches the current code: `drawWaypoints()` is a single `if
(on_screen) {...} else {...}` branch, so a dot and an arrow for the same waypoint are mutually
exclusive by construction, and the default render mode does a full framebuffer redraw every frame
(`full_refresh = 1`, see CLAUDE.md's Render Pipeline section), leaving no stale pixels from a prior
frame to linger. The exact commit that fixed it was not pinned down — most likely an incidental
side effect of the zero-copy render rewrite (moving off the older canvas-based path) rather than a
change targeted at this bug. Root cause of the stale entry: the fix (or the rewrite that subsumed it)
predated this ROADMAP entry being updated to reflect it.

**Key file**: `src/ui/navigation.cpp` — `drawWaypoints()`

---

### Compass Calibration & Tilt — heading is only valid held flat — Resolved (2026-08-02), closed (2026-08-06)
**Was**: the compass is the sole heading source, and the original 2-axis heading formula
(`atan2(cy, cx)`) inverts past **90° − magnetic inclination ≈ 31.5°** of tilt. Facing north held flat,
N pointed to the top — correct. Facing north held vertical (phone-style, deep past the crossover), N
pointed to the **bottom**. WP-3 field data (`docs/calibration/wp3_results.md`) confirmed the error is
heading-dependent, not a fixed bias — at ~46–50° tilt, heading error vs GPS course was **−135° walking
north and +4° walking south** — so no lookup-table correction could fix it, only real
accelerometer-based tilt compensation.

**Resolution**: four staged work packages, WP-0 through WP-6, all shipped 2026-08-02. WP-4 (Level 1
health metrics: `H0`/residual/axis-ratio, runtime `classifyHealth()`, HUD trust indicator). WP-5
(Level 2: a second tumble/figure-8 calibration step recovering a real `cal_z_offset` via 3-D coverage
scoring, min/max-per-axis rather than an ellipsoid fit — [ADR-0019](docs/adr/0019-3-axis-tumble-calibration-not-ellipsoid-fit.md)).
WP-6 (Level 3: accelerometer tilt compensation) — the mag↔accel frame rotation was measured via the
Settings > DEV > Tilt Bench protocol ([`docs/compass_tilt_bench.md`](docs/compass_tilt_bench.md)) and
turned out to be a signed permutation, not the textbook roll/pitch decomposition (which fit the bench
data poorly, ~69° circular std); a coordinate-convention-agnostic vector formula was built instead and
both it and its bench-derived sign were confirmed in live hand-held use — flat, vertical, and
phone-style holding all read correct heading. Full reasoning:
[ADR-0020](docs/adr/0020-tilt-compensation-formula-and-sign-from-bench-data.md).

**Accepted as final, not pursued further (2026-08-06)**: a fast flat→nose-up tilt still produces a
~30° transient heading bounce that self-corrects within ~1s — the gravity EMA
(`GRAVITY_EMA_TAU_S`, tightened 1.0s → 0.5s in response to this exact field report) lagging the
near-instantaneous mag reading mid-transition. This is the accel-only limitation
[ADR-0018](docs/adr/0018-tilt-compensation-required-gyro-deferred.md) flagged as the gyro-fusion
trigger going in — a gyro would fix it, but costs continuous bus traffic against an already-tuned I2C
timing floor (`I2C_PROCESS_MS = 20`, ADR-0013 — see the FT-06/FT-07 entry below for the bus's history
of undiagnosed freezes), an unmeasured power draw, and its own fusion constant to tune. The bounce is
bounded and fast-recovering, not the "persistent lag" ADR-0018 set as the actual trigger, so the
accel-only formula is being kept as-is. **Revisit only if gyro fusion is
reconsidered** — not a scheduled work item. WP-7 (τ-per-zoom smoothing) remains open and independent,
tracked as FT-02 below.

**Also resolved along the way**: `HEADING_SMOOTHING` went 0.8@1Hz → 0.3@10Hz, which did **not**
preserve the time constant (0.62 s → 0.28 s). Restored to α = 0.15 (τ ≈ 0.62 s) and verified on a
walk.

**Full design, field protocol and implementation plan**:
[`docs/compass_calibration_foundation.md`](docs/compass_calibration_foundation.md)

---

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

**Follow-up (2026-08-07)**: the one-time SD→FFat migration described above turned out to have a real
bug — it couldn't distinguish "never migrated" from "user deleted everything on purpose," so an
intentional delete-all silently resurrected old files from SD on the next boot. Fixed, then the
migration mechanism was removed entirely rather than kept as dormant automatic-delete logic. FFat is
now the sole, permanent source of truth for GPX files. Full reasoning: CHANGELOG.md,
[ADR-0027](docs/adr/0027-remove-sd-ffat-gpx-migration.md).

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

### Waypoint Cap: 500 → boot failure → 200 → two-tier index — Resolved (2026-08-05)
**Was**: `MAX_WAYPOINTS` raised 50→500 ([ADR-0022](docs/adr/0022-waypoint-cap-raised-to-500-not-700.md))
shipped, built clean, and passed a static-SRAM budget check — but the very next boot failed:
FreeRTOS task creation returned `pdFAIL`, only 83,899 B internal SRAM free before BLE init once BLE
(~25KB) plus the UI/I2C task stacks (24KB) were granted first. The static-SRAM check alone wasn't
sufficient — it was never validated against actual free heap at boot.

**Resolution**: rolled back to 200 (a comfortable-margin row already in ADR-0022's table),
field-confirmed booting with BLE active. Rather than raise the cap again, added a two-tier index
instead: every waypoint across every GPX file gets a lightweight PSRAM-backed index entry
(`gpx_index`, up to 8192), and the 200-slot SRAM array becomes a working set of the closest-200 to
the user's actual position, reselected as they move — see
[ADR-0023](docs/adr/0023-two-tier-waypoint-index.md). Field-verified on hardware against an
independent Haversine oracle; caught and fixed a real bug along the way (`gpx_loader::init()` was
never called, so the feature was silently dead).

**Still open**: the automatic GPS-driven reselect trigger hasn't been observed firing from real
movement, and concurrent SD access during a reselect is unverified.

**Full reasoning**: [ADR-0022](docs/adr/0022-waypoint-cap-raised-to-500-not-700.md) (superseded on
the cap number, not the render-cost reasoning), [ADR-0023](docs/adr/0023-two-tier-waypoint-index.md),
CHANGELOG.md.

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

**⚠️ One attempted follow-on was tried and reverted**: halving `I2C_PROCESS_MS` (20→10ms) to further reduce the buzzer's timing-quantization floor broke the device on hardware (button unresponsive, buzzer silent) — I2C bus contention with the touch driver, not a CPU cost problem. Reverted and confirmed fixed. `I2C_PROCESS_MS = 20` is now a documented hard floor (see CLAUDE.md's I2C Bus Architecture section).

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

### CRT / 8-bit Display Theme — investigation closed (2026-08-06)
**Was**: the 480×480 IPS panel renders very sharp/clean; ask was a toggleable retro visual style —
scanlines, a coarser/pixelated font or dithering, a subtle phosphor-glow color grade — to evoke a
CRT / 8-bit look, as a **setting** (default stays sharp).

**What the investigation was actually about**: the CRT/8-bit *look* — scanlines and the per-pixel
darken pattern — not the font. The font swap was only ever considered as a fallback if the real
(scanline) route turned out infeasible.

**2026-08-06: scanline/per-pixel route built, tested on hardware, and reverted — rejected on
brightness, not performance.** A halve-brightness darken on alternate output rows was added inline to
`rotate90_tiled`'s existing scatter pass (behind a `rot scanline on|off` serial toggle, no persisted
state, no UI). Measured on real hardware: `tiled rotate` 38.9ms → 42.1ms, frame 80.2ms → 86.2ms
(+6ms, ~7.5%) — a real, non-zero cost (half the destination rows lose the bulk `memcpy` and fall back
to a scalar per-pixel store loop, see ADR-0026), but small enough that it wasn't the blocker. **Killed
by ~20% perceived brightness loss** on a display already run near its readability floor outdoors —
unacceptable regardless of render cost. Code fully reverted (`git diff` clean against the pre-test
commit); nothing of this experiment ships. Full writeup:
[ADR-0026](docs/adr/0026-crt-scanline-brightness-rejected.md).

**Decision: closed, not deferred to the font-only fallback.** A 2026-08-04 scoping pass had sized a
font-only route (LVGL's `LV_FONT_UNSCII_8/16`, 123 `lv_obj_set_style_text_font()` call sites across 7
screen files, layout-risk from UNSCII's fixed pixel size vs. Iosevka's 14/16/20px) as the fallback if
scanlines didn't work out. With scanlines rejected, that fallback was reconsidered on its own and
**won't be pursued** — a font swap alone doesn't deliver the CRT/8-bit *look* the ask was for, and
building it as a standalone feature disconnected from that look isn't worth the 7-screen layout-risk
audit it would require. If a genuine CRT/retro theme is wanted again later, treat it as a new ask and
re-scope from scratch rather than resuming the font-only branch.

---

### GPX Upload/Delete Display Glitch — accepted, not pursued further (2026-08-07)
**Was/is**: every GPX file upload or delete visibly glitches the display (shifted/wrapped frame
content) — a real FFat write/erase briefly disables both cores' cache/interrupts, which can starve
the RGB panel's DMA refill.

**Tried**: `CONFIG_SPI_FLASH_AUTO_SUSPEND` enabled to test as a fix (2026-08-07) — field data suggests
it didn't resolve it.

**Decision**: not pursued further. Documented for the user in the GPX web manager UI (so it reads as
"expected, harmless" rather than a bug report) rather than fixed in the render pipeline. Full detail:
[ADR-0028](docs/adr/0028-defer-gpx-reload-to-explicit-endpoint.md)'s Verification status section,
CHANGELOG.md, and CLAUDE.md's Render Pipeline section.

---

### FT-02: Compass Zoom-Dependent Smoothing
**Was**: At large zoom levels (1km, 5km) compass noise produces visible jitter.
**Original decision (superseded)**: Won't fix — zoom-dependent EMA judged not viable at a 1Hz compass rate. That premise is gone: the compass now samples at 10Hz (see the Compass Calibration & Tilt entry above), the long-term fix this entry called for and never expected to happen without a hardware change. **Not reopened as active work** — it survives only as WP-7 (τ-per-zoom smoothing) in [`docs/compass_calibration_foundation.md`](docs/compass_calibration_foundation.md), independent of the now-closed tilt-compensation work above and not currently scheduled.
