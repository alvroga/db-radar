# Radar Animation Effects — Research from capsule-radar

**Status**: research done, nothing designed or implemented yet.

## Ask

A Claude Code research fork read socquique/capsule-radar's actual rendering source
(github.com/socquique/capsule-radar, LVGL-based, plain `lv_obj` + custom `LV_EVENT_DRAW_MAIN` draw
callbacks — architecturally close to db-radar's own `radarDrawEventCb`) to see which of its visual
effects could realistically port over.

## Two candidates identified as directly implementable, no architecture change

- **Sweep beam** (their `sweep_draw_cb`) — the rotating scan line with a soft fading trail, seen in
  their demo gif. Not a gradient or image: ~20 separate straight lines from center to rim, each at a
  slightly earlier angle than the last, opacity fading *quadratically* (`frac² × max_opacity`) rather
  than linearly for a soft comet-tail look. One `float` angle variable advances each tick
  (`angle += 360° × frame_time / period`). Pure `lv_draw_line()` calls, no `lv_canvas`, no gradients —
  compatible with db-radar's no-canvas / `clip_corner`-off / `full_refresh=1` constraints as-is.
  **The most promising candidate to actually prototype and measure.**
- **Sonar/glow rings** (their `draw_ball`) — 3 staggered `lv_draw_arc` circles at increasing radius
  and decreasing opacity, driven by one shared phase variable (~0.34 apart, incrementing ~0.05/tick).
  Candidate for the beacon ball or a fixed waypoint's "locked on" indicator. Same no-canvas
  compatibility as the sweep beam.

## What does NOT port over, and why

Their whole perf strategy rests on **partial invalidation** — `lv_obj_invalidate_area()` on just the
changed region, so their sweep/blips cost almost nothing per frame. db-radar's `full_refresh=1` is
load-bearing (the tiled-transpose 90° rotation rewrites the entire back framebuffer every flush, see
ADR-0004/ADR-0008) and cannot be relaxed without giving back the 2.5×+ rotation-speed win that
constraint exists for — so any of the above, if added, costs its full paint time every single frame,
not a bounded delta the way it does for them. Their persistent `lv_canvas`-based "flow" heatmap layer
is a hard no regardless — directly contradicts the no-canvas render design and would need its own ADR
to even reconsider.

## Before implementing — measure first

CLAUDE.md's Render Pipeline section / `docs/performance_optimization_backlog.md` break down where the
current ~94ms frame goes (rotate ~38ms, non-radar LVGL draw ~17ms, radar bg fill ~20.5ms, radar paint
~9.3ms for *all* current geometry combined). A sweep beam's ~20 line draws are cheap in isolation, but
"cheap" needs an actual `esp_timer_get_time()` bracket around the new draw calls on real hardware
before/after, per the project's own "residual trap" methodology (never attribute an un-instrumented
remainder) — not an assumption from the line count alone.

## Open questions to resolve before design

- Where does the sweep beam live visually in db-radar's radar (heading-up mode already rotates the
  whole scene with the compass — does a sweep read as redundant/confusing layered on top of that, or
  does it work fine since it's a separate independent rotation)?
- Sweep period/speed — their demo's cadence isn't necessarily the right one for a GPS/compass radar
  used outdoors at a glance, not stared at continuously.
- Whether the sonar-ring effect is worth adding to the beacon ball specifically, given the beacon
  proximity system already has its own dedicated ring gauge + buzzer sonar (`docs/beacon_proximity.md`)
  — could be redundant with an existing signal rather than additive.

## Status

Nothing implemented, nothing designed — this document is the research only. Sweep beam is the one to
actually prototype and measure first if this gets picked up.
