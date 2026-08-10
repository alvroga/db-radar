# ADR-0006: Full-frame (480-line) LVGL draw buffers

Status: Accepted
Date: 2026-03-20
Decided by: Claude (backfilled from project history 2026-07-31)

## Context

LVGL's draw buffers had climbed incrementally — 40 → 50 → 120 → 160 lines — chasing a visible
top-to-bottom "wipe" artifact during screen transitions and a reduction in flush count (480÷160 = 3
flushes per frame, vs 10 at 50 lines; `CHANGELOG.md:1536-1551`). At the time this decision was made,
the render path used LVGL's built-in `sw_rotate = 1` (see ADR-0004 — the tiled-transpose replacement
was still four months away) and painted the radar into an `lv_canvas` (ADR-0005 hadn't happened
either). Under that older pipeline, a partial-height draw buffer meant the software rotation step
operated on incomplete frame data mid-transition, which is what produced the wipe: going to a
480-line buffer — the full frame in one buffer — removed the partial state entirely and dropped flush
count to 1 per frame (`CHANGELOG.md:867-874`).

## Decision

Set `BUFFER_LINES = 480` (`include/core/system_config.h:34`) — a full-frame LVGL draw buffer, sized
480×480×2 bytes × 2 buffers = 921KB PSRAM — instead of continuing to tune a partial-height buffer.

## Consequences

**Easier**: eliminated the wipe artifact outright and reduced flush operations to one per frame, at
the cost the incremental approach had been trying to avoid paying all at once.

**Harder**: 921KB of PSRAM committed to draw buffers alone, on top of whatever the panel's own
framebuffers cost (ADR-0007) and the radar's own allocations at the time (ADR-0005 later freed 460KB
of those). This was 11.5% of the board's 8MB PSRAM for buffering alone at introduction.

**Gave up / superseded**: the original justification — "eliminates transition wipe artifact **with
software rotation**" — stopped being the operative reason on 2026-07-28, when `sw_rotate` was replaced
by the tiled transpose (ADR-0004) and TILED became the default mode with `sw_rotate` off. CLAUDE.md's
"Render Pipeline" section explicitly flags the old rationale as one of three claims in the superseded
optimization write-up that are "now false," while separately listing `BUFFER_LINES = 480` itself among
what "is still true" — the value survived the pipeline rewrite around it, but no fresh justification
for keeping it at 480 (rather than re-tuning against the current zero-copy path) has been written down
since. That is an open gap, not a re-confirmed decision — flag it if display memory pressure ever
becomes a reason to revisit.
