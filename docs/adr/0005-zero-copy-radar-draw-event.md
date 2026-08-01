# ADR-0005: Radar paints via `LV_EVENT_DRAW_MAIN`, not an `lv_canvas` blit

Status: Accepted
Date: 2026-07-28
Decided by: Claude (backfilled from project history 2026-07-31)

## Context

The radar had always been drawn into a full-screen `lv_canvas`: `updateRadarDisplay()` painted
geometry into a 480×480 PSRAM buffer, and LVGL treated that buffer as an image, blitting it into the
draw buffer on every refresh. By the point this was reconsidered (2026-07-28), frame time had already
been cut from ~499ms to 238ms via earlier canvas-clear and rotation fixes, and the canvas blit itself
was the next largest attributable cost — the backlog's projection (`refr − rot − flush` assumed to be
entirely the blit) put it near 104ms (`CHANGELOG.md:606-607`, `docs/performance_optimization_backlog.md`
§2.1/C4). The canvas also held a permanent 460KB PSRAM allocation whether or not the radar screen was
even visible.

## Decision

Drop the `lv_canvas` and its intermediate buffer entirely. Replace it with a plain `lv_obj`
(`radar_obj`) that paints itself from an `LV_EVENT_DRAW_MAIN` event handler
(`navigation::radarDrawEventCb`), emitting geometry straight into LVGL's draw context — no
intermediate image buffer to allocate, clear, or blit. Shipped in commit `816b421` (2026-07-28).
Measured: frame 238 → 210ms, 460KB PSRAM freed (`CHANGELOG.md:601-604`).

## Consequences

**Easier**: one fewer full-screen buffer in the memory budget (460KB PSRAM back), one fewer copy per
frame, and `updateRadarDisplay()` is simplified to refreshing HUD labels and requesting an invalidate
rather than owning the paint itself (see the "Render Pipeline" section of CLAUDE.md).

**Harder**: painting now happens *inside* LVGL's refresh instead of as a separate, independently
timeable phase, which changed how the on-screen HUD/`perf` command has to attribute time (`paint` is
now a component of `refr`, not sequential with it — CLAUDE.md "Timing semantics").

**Gave up**: `lv_obj_create()` sets `LV_OBJ_FLAG_CLICKABLE` by default where `lv_canvas_create()` did
not (`lv_obj.c:436`). This silently made the new radar surface win hit-testing and swallow every
touch before it reached the stage handler that calls `handleTapAt()` — waypoint detail taps stopped
working, invisibly, because the radar still rendered identically. This regression shipped in the same
commit and was caught and fixed in `44f6d0d` by clearing `LV_OBJ_FLAG_CLICKABLE` on `radar_obj`
(`src/ui/ui_manager.cpp:251-257`; `CHANGELOG.md:636-643`). It is now one of the load-bearing invariants
of this render path — see ADR-0008.
