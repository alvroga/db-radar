# Architecture Decision Records

Started 2026-07-31. An ADR captures *why this option and not the others* for a decision that had
genuine alternatives — not every change, and not "what we built" (that's CHANGELOG.md and the
component docs in `docs/`).

## When to write one

Write an ADR when a future reader could reasonably ask "why didn't they just—". Examples from this
project that would qualify: software vs hardware display rotation, passive vs active BLE scanning,
zero-copy `lv_obj` drawing vs `lv_canvas`, the `I2C_PROCESS_MS = 20` floor. A bug fix, a tuned
constant with one obvious value, or routine feature work does not need one.

## Format

Copy `0000-template.md`, number sequentially (`0001-`, `0002-`, ...), keep it short — Context /
Decision / Consequences, a few sentences each. Link related ADRs and `docs/*.md` pages by relative
path.

**Numbers are stable IDs, assigned in creation order — never renumbered.** When the historical
backfill (see below) eventually runs, its ADRs get appended after whatever number forward-authored
ADRs have reached by then; they are not inserted earlier in the sequence and nothing gets
renumbered to make file order match decision order. Each ADR's `Date:` field carries the real
decision date, so sort on that field for chronological order — not on the filename. This is so a
citation like "ADR-0007" never breaks.

## Index

Sorted by number (creation order). The `Date:` field inside each file carries the real decision
date — sort on that for chronology, not on the filename.

| # | Decision | Date | Status |
|---|---|---|---|
| [0001](0001-waypoint-detail-psram-cache.md) | Cache waypoint desc/hint in PSRAM rather than re-reading the GPX file on tap | 2026-07-31 | Accepted |
| [0002](0002-serial-logging-default-independent-of-dev-mode.md) | Serial logging defaults ON, independent of `dev_mode` | 2026-07-31 | Accepted |
| [0003](0003-proactive-i2c-bus-recovery-watchdog.md) | Proactive I2C bus-recovery watchdog, not per-failure or task-hang-triggered recovery | 2026-07-31 | Accepted |
| [0004](0004-tiled-transpose-display-rotation.md) | Tiled transpose in the flush callback, not LVGL `sw_rotate` | 2026-07-28 | Accepted |
| [0005](0005-zero-copy-radar-draw-event.md) | Radar paints via `LV_EVENT_DRAW_MAIN`, not an `lv_canvas` blit | 2026-07-28 | Accepted |
| [0006](0006-full-frame-lvgl-draw-buffers.md) | Full-frame (480-line) LVGL draw buffers | 2026-03-20 | Accepted |
| [0007](0007-dual-panel-framebuffers-zero-copy-flush.md) | Dual panel framebuffers with zero-copy flush | 2026-07-28 | Accepted |
| [0008](0008-zero-copy-render-path-invariants.md) | Accept the zero-copy render path's four load-bearing invariants | 2026-07-28 | Accepted |
| [0009](0009-nimble-instead-of-bluedroid.md) | NimBLE instead of Bluedroid | 2026-03-20 | Accepted |
| [0010](0010-continuous-passive-ble-scan.md) | Continuous passive BLE scan instead of active stop/restart | 2026-07-31 | Accepted |
| [0011](0011-continuous-mappings-input-filtering.md) | Continuous mappings with input-side filtering, instead of discrete zones with output-side hysteresis | 2026-07-31 | Accepted |
| [0012](0012-beacon-priority-over-waypoint-sonar.md) | Beacon takes absolute priority over the fixed-waypoint sonar | 2026-07-31 | Accepted |
| [0013](0013-i2c-process-ms-tuned-floor.md) | `I2C_PROCESS_MS = 20` is a tuned floor, not an arbitrary interval | 2026-07-31 | Accepted |
| [0014](0014-compass-stays-on-shared-i2c-bus.md) | Compass stays on the shared I2C bus rather than moving to a second bus | 2026-03 | Accepted |
| [0015](0015-body-shadow-direction-finding-not-aoa.md) | Body-shadow direction finding instead of BT 5.1 Angle-of-Arrival | 2026-07-31 | **Proposed** |
| [0016](0016-freertos-multitask-architecture.md) | FreeRTOS multi-task architecture instead of a single Arduino loop | 2025-10 | Accepted |
| [0017](0017-compass-sole-heading-source.md) | Compass as the sole heading source, replacing GPS heading fusion | 2026-03-20 | Accepted |
| [0018](0018-tilt-compensation-required-gyro-deferred.md) | Real tilt compensation required (not a lookup table); gyro fusion deferred, not adopted | 2026-08-02 | Accepted |
| [0019](0019-3-axis-tumble-calibration-not-ellipsoid-fit.md) | 3-axis calibration via a second min/max tumble step, not an ellipsoid/soft-iron fit | 2026-08-02 | Accepted |
| [0020](0020-tilt-compensation-formula-and-sign-from-bench-data.md) | Tilt-compensation frame rotation and sign resolved from bench self-consistency, not the textbook formula or a first-principles sign argument | 2026-08-02 | Accepted |
| [0021](0021-i2c-nack-hang-build-time-backport.md) | Build-time backport of the IDF NACK-hang fix, not a version bump or a manual edit | 2026-08-02 | Accepted |
| [0022](0022-waypoint-cap-raised-to-500-not-700.md) | Raise `MAX_WAYPOINTS` to 500 (not 700+), after replacing Haversine with equirectangular | 2026-08-05 | Superseded on the cap number (see ADR-0023) |
| [0023](0023-two-tier-waypoint-index.md) | Two-tier waypoint index (PSRAM full index + SRAM working set) | 2026-08-05 | Accepted |
| [0024](0024-ota-partitions-grown-from-unused-ffat.md) | OTA partitions grown to 4MB by reclaiming unused FFat; GPX storage moving off SD | 2026-08-06 | Accepted, revised same day (3.5MB → 4MB) |
| [0025](0025-version-scheme-monthly-build-counter.md) | `FW_VERSION` monthly build counter recovered from the committed header, not a separate state file | 2026-08-06 | Accepted |
| [0026](0026-crt-scanline-brightness-rejected.md) | CRT scanline effect built and measured, rejected on brightness | 2026-08-06 | Rejected — reverted, no code shipped |
| [0027](0027-remove-sd-ffat-gpx-migration.md) | Remove SD→FFat GPX auto-migration entirely | 2026-08-07 | Accepted |
| [0028](0028-defer-gpx-reload-to-explicit-endpoint.md) | GPX upload/delete no longer auto-reload; client calls `/reload` once per batch | 2026-08-07 | Accepted |
| [0029](0029-custom-canvas-fixed-waypoint-icon.md) | Custom `lv_canvas` icon for the fixed-waypoint status indicator, not an `LV_SYMBOL_*` glyph or image asset | 2026-08-07 | Accepted |
| [0030](0030-release-pipeline-build-time-pages-not-committed-binaries.md) | Release pipeline builds once, publishes to both GitHub Releases and GitHub Pages — no binaries committed to git | 2026-08-08 | Accepted |
| [0031](0031-single-part-manifest-not-multi-part.md) | ESP Web Tools manifest uses one merged-binary part at offset 0, not four separate parts | 2026-08-08 | Accepted |
| [0032](0032-pinned-gps-module-not-always-auto-detect.md) | Pinned GPS module selection (first-boot picker), not always-auto-detect — 2026-08-11 addendum adds BN-880 as a third option | 2026-08-10 | Accepted |
| [0033](0033-compass-internal-dispatch-not-rename.md) | Compass internal chip dispatch (QMC5883L/HMC5883L), not a public namespace rename | 2026-08-11 | Accepted |
| [0034](0034-touch-nack-throttle-not-permanent-breaker.md) | Touch NACK handling: throttled retry + DisAutoSleep re-arm, not a permanent circuit breaker; wedge detector needs ≥2 failing devices | 2026-08-21 | Accepted |

## Status

- **Going forward**: new architectural decisions get an ADR at the time they're made (see CLAUDE.md
  → Documentation Standards). Next free number: **0035**.
- **Historical backfill**: ✅ complete (2026-08-01). ADRs 0004–0017 were reconstructed from
  CHANGELOG.md, ROADMAP.md, `docs/performance_optimization_backlog.md`, the component docs and git
  history. They carry
  `Decided by: Claude (backfilled from project history 2026-07-31)` so a reader can tell a
  reconstruction from a decision recorded as it was made.

  **Read backfilled ADRs with one caveat**: their reasoning was recovered from prose written at
  various times, and three of them record reasoning now known to be *stale or unverified* rather
  than settled — 0006 (the 480-line buffer's original justification expired when the render pipeline
  was rewritten around it, and no fresh one has been written), 0013 (10ms broke the device on
  hardware, but the root cause was never diagnosed) and 0014 (the original "compass can't share the
  bus" reasoning describes the pre-ESP-IDF Arduino build and has never been retested). Each says so
  in its own text. That is the honest state of the record, not an omission to be tidied away.
