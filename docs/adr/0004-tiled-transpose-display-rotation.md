# ADR-0004: Tiled transpose in the flush callback, not LVGL `sw_rotate`

Status: Accepted
Date: 2026-07-28
Decided by: Claude (backfilled from project history 2026-07-31)

## Context

The panel is mounted 90° CCW in the enclosure — required so the GPS module clears the enclosure
indent and has sky visibility (ROADMAP, commit `ff82116` message) — so the image must be rotated 90°
before it reaches glass. That rotation cannot happen in the display hardware: the ST7701 in RGB mode
has no line buffer for MADCTL `MV`, so it can do a 180° flip but not a 90° swap
(`docs/performance_optimization_backlog.md:1727-1729`; confirmed independently in the `ff82116`
commit message: "ST7701 in RGB mode cannot do 90 in hardware"). Software rotation was therefore not
one option among several from the start — it was the only one available — and shipped as
`disp_drv.sw_rotate = 1` / `LV_DISP_ROT_90` in v0.11.0 (2025-10-21, CHANGELOG).

By 2026-07-28 that software rotation was the single largest cost in a 499ms frame. LVGL's
`draw_buf_rotate_90_sqr` walks points 960 bytes apart, missing the data cache on ~3 of 4 accesses —
measured at 162ms/frame by A/B (284ms refresh with `sw_rotate=1` vs 122ms without,
`docs/performance_optimization_backlog.md:1722-1725`, `ff82116`). Three real alternatives were
weighed at that point, not two:
- **Option A — tiled transpose in the flush callback**, keeping source rows and destination columns
  resident in the 32KB dcache instead of striding.
- **Option B — pre-rotate the drawn geometry** instead of the pixels. Free for the radar's
  code-generated geometry, but LVGL 8.x has no rotated-label support, so HUD/settings/waypoint text
  would render sideways. Ruled out.
- **Option C — remount the panel** in its native orientation. Zero software cost, but blocked by the
  same enclosure/GPS constraint that caused the 90° mount in the first place. Ruled out.
(`docs/performance_optimization_backlog.md:1713-1748`, explicitly: "Neither is available; do not
re-propose them.")

## Decision

Replace LVGL's built-in `sw_rotate` with a hand-written 32×32 blocked transpose in `lvgl_flush_cb`
(`rotate90_tiled`, `src/core/device_manager.cpp`), keeping both source and destination tiles resident
in cache instead of striding through PSRAM. Shipped in commit `ff82116` (2026-07-28): rotation
162 → 64.3ms (2.5×), full frame ~317 → ~225ms.

## Consequences

**Easier**: rotation cost dropped by more than half immediately, and further tuning in the same
session (§9b/§9 of the backlog, ADR-0007) took it to 47.4ms. Restores LVGL double buffering — `sw_rotate`
had forced a serial render→flush wait that the tiled path removes. The mode is runtime-switchable
(`rot on|off|tiled`, `src/utils/diagnostics.cpp:125-145`, `device_manager::requestRotMode()`), which
turned this from a one-shot change into a standing A/B harness for every rotation-adjacent
optimization that followed.

**Harder**: the transpose is now a hand-maintained function instead of a library call, and it owns
`full_refresh`'s value (ADR-0008) rather than being independent of it — the two config sites can
disagree if edited separately (noted as a tidying item in the backlog, not yet done). Two staging
buffers cost +921KB PSRAM at introduction (later eliminated by ADR-0007's zero-copy flush).

**Gave up**: nothing that was available — Options B and C were not durable alternatives, they were
ruled out by LVGL's text-rendering model and the physical enclosure respectively. The real cost paid
here is engineering effort for a hand-rolled cache-aware kernel in place of a one-line library flag.
