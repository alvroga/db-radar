# ADR-0008: Accept the zero-copy render path's four load-bearing invariants

Status: Accepted
Date: 2026-07-28
Decided by: Claude (backfilled from project history 2026-07-31)

## Context

The 2026-07-28 render rewrite (ADR-0004 tiled transpose, ADR-0005 zero-copy `lv_obj` painting,
ADR-0007 dual-framebuffer flush) took the radar frame from ~499ms to ~94ms, but in doing so introduced
four properties of the surrounding code that are correct only because of *how* that path is wired —
each looks like a harmless cleanup target to someone who doesn't know the pipeline underneath it, and
each was either a measured regression or a silent correctness bug when it was gotten wrong the first
time. CLAUDE.md's "Render Pipeline" section calls these out explicitly as "load-bearing — do not
'clean these up.'" Documenting them once, with their cost, is cheaper than re-deriving (or
re-breaking) each one every time the code around it is touched.

## Decision

Accept and document these four invariants as the fixed price of the zero-copy render path, rather than
re-deriving or "simplifying" them later:

1. **`clip_corner` stays OFF on the radar stage.** LVGL answers `LV_EVENT_COVER_CHECK` with
   `LV_COVER_RES_MASKED` for any object with `clip_corner` set, and `lv_refr_get_top_obj` treats
   `MASKED` as *stop, do not descend* — so turning it on makes LVGL repaint the screen and stage
   backgrounds beneath the radar every frame (**+61ms**) and installs a radius mask every child draw
   call blends through (**grid 3× slower**, 20–26ms → 6–9ms). The panel is physically round, so the
   corner-clipping it would provide is invisible anyway. Found and fixed in `44f6d0d`
   (`src/ui/ui_manager.cpp:65,107,191-194`; `docs/performance_optimization_backlog.md` §2.3/C5).

2. **`radar_obj` must not be `CLICKABLE`.** `lv_obj_create()` sets that flag by default where
   `lv_canvas_create()` did not (`lv_obj.c:436`) — with it set, the radar surface wins hit-testing and
   silently swallows every touch before it reaches the stage handler's `handleTapAt()`. The radar
   renders identically either way, so the failure is invisible until someone taps a waypoint. Fixed in
   `44f6d0d` by clearing the flag (`src/ui/ui_manager.cpp:251-257`).

3. **`full_refresh` tracks the rotation mode, not a fixed value** — `1` whenever the zero-copy
   TILED path is active, `0` otherwise (`src/core/device_manager.cpp:624,645,980`). The transpose
   rewrites the *entire* back framebuffer; a partial flush area would leave the rest of it holding a
   two-frames-old image. It must move with the runtime `rot` switch (ADR-0004) because LVGL rejects
   `full_refresh` together with `sw_rotate`.

4. **The `on_frame_buf_complete` guard is not dead code.** The driver latches
   `bb_fb_index = cur_fb_index` only at a frame boundary (`src/core/device_manager.cpp:916`), so
   between a framebuffer swap and that latch the "back" buffer is still being scanned out to the
   panel. At 94ms/frame against a 26.6ms panel period it never actually blocks today — it exists so
   that raising PCLK or shaving the frame further (ADR-0007's Consequences) cannot silently
   reintroduce tearing.

## Consequences

**Easier**: each invariant now has one canonical place it's explained, with its measured cost, instead
of living only in the memory of whoever wrote the 2026-07-28 commits. A future contributor who wants
to touch `clip_corner`, `radar_obj`'s flags, `full_refresh`, or the framebuffer-complete callback has a
number to check against before assuming their change is a harmless simplification.

**Harder**: the render path has more implicit coupling between unrelated-looking pieces of code
(a style flag, an object flag, a driver config bool, and an RGB panel callback) than a
straightforward LVGL setup would. Four separate places must each be gotten right, and getting any one
wrong degrades performance or correctness silently rather than failing loudly.

**Gave up**: nothing new — items 1 and 2 were regressions caught and fixed within the same day they
were introduced (`44f6d0d`), not alternatives that were deliberately traded away. This ADR's decision
is to stop treating them as one-off bug fixes and instead carry them forward as permanent constraints
on this render path.
