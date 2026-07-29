# Performance Optimization Backlog

**Status**: Largely implemented. **Frame ~499 ms → 94 ms (~0.8 → ~10 fps).** See "Completed"
below for what shipped and the measured breakdown for where the remaining 94 ms sits. Sections 0–5
are the **original 2026-07-27 analysis, preserved as written** — several of their conclusions were
disproved by measurement, and the ones that were are flagged inline. Trust the ✅ sections over
them.
**Date**: 2026-07-27 (measurements + completions: 2026-07-28)

---

## ✅ COMPLETED

### C1. `lv_canvas_fill_bg` → `lv_color_fill` (§6 step —, was priority 1) — 2026-07-28

Commit `fa63a03`. Measured: **`fill_bg` 205.0 → 21.3 ms (9.6×)**, paint stage 215.3 → 32.2 ms (6.7×).
Full before/after numbers in the measured-results section below.

### C2. Decoupled heading rotation from the 1 Hz GPS redraw (§3.2, §6 step 11) — 2026-07-28

**This was the "payoff" item, and it turned out not to require steps 7–10 first.**

The premise of §0 point 1 was right: `processUIUpdate()` skipped the compass redraw whenever GPS had
a fix, so rotation ran at the 1 Hz `RADAR_REFRESH` rate instead of the 5 Hz compass rate. The guard's
own comment claimed `RADAR_REFRESH` "is queued in the same burst" as `COMPASS_UPDATE` — true only 1
time in 5, since the compass read runs every 200 ms System Task tick while the GPS read is gated by
`GPS_UPDATE_INTERVAL_MS = 1000`.

Fix: **coalesce rather than suppress.** All four render-triggering cases (`RADAR_REFRESH`,
`COMPASS_UPDATE`, both `ZOOM_CHANGE`s) now call `requestRadarRender()`, which only sets a flag. The
UI Task calls `flushRadarRender()` once after draining the queue batch, still inside `display_mutex`.

- Rotation now tracks the fastest producer — **1 Hz → 5 Hz with a fix**.
- **§3.3 is resolved as a side effect.** That item warned of `4 × 149 ms` of queued renders with the
  mutex held; renders per UI loop are now capped at **1**, strictly fewer than before.
- Verified on hardware with a 14-satellite fix: rotation smooth, button remains responsive.

**Why this landed before the expensive items**: the frame was already cheap enough after C1 — the
problem was that there were 5× fewer frames, not that each one cost too much. C1 + C2 together were
XS effort and delivered the felt result that steps 7–10 were budgeted for.

**Files**: `src/utils/task_manager.cpp` — `requestRadarRender()` / `flushRadarRender()`, forward
decl at :66, flush at :175, cases at :541/:559/:567/:687.

### C3. Tiled transpose, `sw_rotate` off (§2.2 A, step 7) — 2026-07-28

Commit `ff82116`. Measured: **rotation 162 → 64.3 ms (2.5×)**. 32×32 blocked transpose in
`lvgl_flush_cb`, keeping source rows and destination columns resident in the 32 KB dcache instead of
striding 960 bytes between every pixel. Staging buffers are paired by pointer to LVGL's two draw
buffers, so they inherit LVGL's own "never re-render into a buffer with an outstanding flush"
guarantee.

Runtime-switchable via `rot on|off|tiled` for A/B measurement. **Touch requires that *something*
rotates the pixels** — LVGL's input transform keys off `rotated` alone and assumes the framebuffer
was rotated to match, so `RotMode::NONE` leaves the UI visible but untouchable. That is a
measurement mode only, never a shipping configuration.

### C4. Drop the radar canvas → `LV_EVENT_DRAW_MAIN` (§2.1, step 8) — 2026-07-28

Commit `816b421`. Measured: **frame 238 → 210 ms**, plus **460 KB of PSRAM freed**.

Far less than the ~104 ms this document projected, because that projection was a residual (see "The
residual trap"). The canvas blit was worth ~22 ms; the rest of the bucket was LVGL work the change
does not touch. The PSRAM saving is real and was never in doubt.

Also introduced one regression, caught on hardware: `lv_obj_create()` sets `LV_OBJ_FLAG_CLICKABLE`
(`lv_obj.c:436`) where `lv_canvas_create()` does not, so the new radar surface became the hit-test
winner and swallowed presses before they reached the stage handler calling `handleTapAt()`. Waypoint
detail taps stopped working. Fixed in `44f6d0d` by clearing the flag — the surface is for painting,
input belongs to the stage beneath it.

### C5. `clip_corner` was defeating LVGL's cover-check (step 8b) — 2026-07-28

Commit `44f6d0d`. Measured: **frame 210 → 149 ms**, the single largest win of the effort.

Found by bracketing the radar background fill with a `DRAW_MAIN_BEGIN` timestamp, which split the
opaque 82 ms remainder into `bg 23 ms` + `non-radar 62 ms`. The 62 ms was two full-screen background
fills — the screen's and the stage's — painted every frame, both opaque, both the same green, both
immediately covered by the radar.

Cause: `lv_obj_set_style_clip_corner(stage, true)`. LVGL answers `LV_EVENT_COVER_CHECK` with
`LV_COVER_RES_MASKED` for any object with `clip_corner` set, and `lv_refr_get_top_obj` treats
`MASKED` as *stop, do not descend into children*. The search for the topmost fully-covering object
bailed at the stage and never reached `radar_obj`, so LVGL fell back to drawing from the screen down.

`clip_corner` also installs a radius mask on the draw context that **every child draw call** blends
through — which is what made grid drawing 3× more expensive (20–26 → 6–9 ms) after step 8 moved
painting inside the stage. One flag, both symptoms.

Nothing lost visually: the panel is physically round, so the clipped corners are not on the glass.

### C6. Transpose tuning (step 9b) — 2026-07-28

Commit `311ca3c`. Measured: **rotation 64.1 → 55.7 ms**, short of the projected ~40 ms.

`rotate90_tiled` gained `IRAM_ATTR` and an internal-SRAM scratch tile. Tiling alone still left one of
the two PSRAM streams strided — whichever loop is innermost gets sequential access and the other hops
by a 960-byte row stride. Transposing *via* a 2 KB SRAM tile makes both PSRAM sides sequential: read
a source row run, scatter into SRAM (not cached on the S3, so free), emit each destination row run as
one `memcpy`.

**Why the ~40 ms projection was wrong, and how it could have been known in advance.** The answer was
already in the step-8 measurements: `esp_lcd_panel_draw_bitmap` moved the same 460 KB PSRAM→PSRAM
with an optimized `memcpy` in 34 ms — **27 MB/s**, a clean upper bound for this hardware on this
access pattern. The transpose after tuning runs at ~16.5 MB/s. Real remaining headroom was ~1.6×, not
1.6× *on top of* what tiling had already delivered. The ~40 ms number was another unmeasured guess;
the bound to check it against was sitting two tables away.

### C7. `num_fbs = 2`, zero-copy flush (§2.3, step 9) — 2026-07-28

Commit `311ca3c`. Measured: **flush 34.0 → 0.02 ms** — deleted, not reduced — and **frame 145 → 94 ms**.

The panel allocates two framebuffers. `rotate90_tiled` writes into the back one and hands that
pointer to `esp_lcd_panel_draw_bitmap`, which scans its own framebuffer list, recognises the pointer,
and just sets `cur_fb_index` (`esp_lcd_panel_rgb.c:614-624`). The staging-buffer write and the
full-screen memcpy both disappear in one change.

The revised plan in §2.3 was the right shape — `full_refresh` + `direct_mode` as originally written
would not have worked, because the pixels still have to be rotated somewhere.

**Rotation also fell, 55.7 → 47.4 ms.** Nothing about the transpose changed; the flush memcpy had
been competing with it for PSRAM bandwidth and cache. This reversed a decision made during 9b: a
64-pixel tile beat 32 by 3.4 ms *while the flush existed*, and by 0.5 ms (noise) once it was gone. The
tile went back to 32 and kept 6 KB of SRAM. **Tuning constants measured against a pipeline you are
about to change are provisional — re-measure them after.**

**PSRAM −460 KB net**: +460 KB for the second framebuffer, −920 KB from no longer allocating the
rotation staging buffers at all.

Two constraints this introduced, both in `device_manager.cpp`:

1. **`full_refresh = 1` is load-bearing for the zero-copy path.** A partial flush area would leave
   the rest of the alternate framebuffer holding a two-frames-old image. It moves in lockstep with
   the rotation mode in both `initLVGL()` and `applyPendingRotMode()`, because LVGL rejects
   `full_refresh` together with `sw_rotate` — so `rot on` must clear it.
2. **`on_frame_buf_complete` guards the transpose.** The driver latches `bb_fb_index = cur_fb_index`
   only at a frame boundary (`esp_lcd_panel_rgb.c:831-834`), so between a swap and that latch the
   "back" buffer is still the one being scanned out. At 94 ms/frame against a 26.6 ms panel period
   this never blocks — it exists so step 10 cannot silently reintroduce tearing.

### Still open

Step 10 (higher PCLK, §2.4) and the Tier 0 build-config items, none of which have been attempted.
Rotation at 47.4 ms is now half the frame and is close to its ceiling for this approach — see the
measured breakdown below.

---

## ⚠️ MEASURED RESULTS — 2026-07-28 (supersedes the estimates below)

On-device measurement via the DEV render HUD (`perf` serial command). **Build: rotation DISABLED**
(`-DRADAR_ROTATION_DEGREES=0`), 480×480, no GPS fix, indoors:

```
--- paint stage (updateRadarDisplay) ---
  fill_bg:          205018 us  (205.0 ms)   <-- 59% of the entire frame
  grid:               4403 us  (  4.4 ms)
  waypoints:          3594 us  (  3.6 ms)
  triangle+N+gauge:   1926 us  (  1.9 ms)
  other:               314 us  (  0.3 ms)
  PAINT TOTAL:      215255 us  (215.3 ms)
--- refresh stage (LVGL blit + rotate + flush) ---
  REFRESH:             131 ms   (230400 px)
FRAME TOTAL:         346.3 ms
```

### What this overturns

1. **Software rotation was NOT the bottleneck.** §2.2 was the headline recommendation and it is
   wrong. With `sw_rotate` fully disabled the frame still costs **346 ms** — worse than the ~149 ms
   this document estimated *with* rotation. The user's eyeball test ("looks the same") was correct.
   The four-PSRAM-passes model in §0 was directionally sound but misattributed the cost.

2. **The real bottleneck is `lv_canvas_fill_bg` — 205 ms in a single call that just clears the
   canvas to one colour.** Root cause is in LVGL itself
   (`.pio/libdeps/cc-radar/lvgl/src/widgets/lv_canvas.c`): for `LV_IMG_CF_TRUE_COLOR` it takes the
   per-pixel `else` branch:

   ```c
   for(y = 0; y < dsc->header.h; y++)
       for(x = 0; x < dsc->header.w; x++) {
           lv_img_buf_set_px_color(dsc, x, y, color);
           lv_img_buf_set_px_alpha(dsc, x, y, opa);
       }
   ```

   That is **460,800 out-of-line function calls per frame** (230,400 px × 2), each doing a colour-format
   switch plus pointer arithmetic, writing into PSRAM. The 1-bit formats get a fast `lv_memset`;
   true-colour does not. Effective throughput: 460 KB / 205 ms ≈ **2.25 MB/s**, roughly 20× below
   what the PSRAM bus can sustain.

3. **All the canvas draw-call concerns were negligible.** Grid + waypoints + triangle + north +
   beacon gauge together cost **10.2 ms**, ~3% of the frame. §3.1 (grid lines as rects) and most of
   §2.1 (delete the canvas) target noise, not signal.

### Result after fixing `fill_bg` (2026-07-28, rotation back ON)

`lv_canvas_fill_bg()` replaced with `lv_color_fill()` over `dsc->data` in `navigation.cpp`:

```
  fill_bg:           21315 us  ( 21.3 ms)   <-- was 205.0 ms
  grid:               4737 us  (  4.7 ms)
  waypoints:          3608 us  (  3.6 ms)
  triangle+N+gauge:   2133 us  (  2.1 ms)
  PAINT TOTAL:       32178 us  ( 32.2 ms)   <-- was 215.3 ms
  REFRESH:             284 ms   (230400 px) <-- rotation ON
FRAME TOTAL:          316.2 ms
```

**`fill_bg` 205.0 → 21.3 ms (9.6×). Paint stage 215.3 → 32.2 ms (6.7×).**

### Rotation: the earlier "not the bottleneck" verdict was itself wrong

Comparing the two measured builds isolates rotation cleanly, because paint is now small:

| Build | Rotation | paint | refresh | frame |
|---|---|---|---|---|
| A | OFF | 215.3 ms | **131 ms** | 346.3 ms |
| B (fill fixed) | ON | 32.2 ms | **284 ms** | 316.2 ms |

Same 230,400 px in both refresh figures, so **software rotation costs ~153 ms/frame**. The original
§2.2 hypothesis was right; the rotation-OFF A/B test that appeared to disprove it was *confounded* —
`fill_bg`'s 205 ms dominated the frame and masked the 153 ms that rotation was contributing. Removing
rotation did help, it just moved a number that was hidden behind a bigger one.

Projected: fixing rotation **as well** gives 32.2 + 131 ≈ **163 ms/frame**, from the ~499 ms this
configuration would have cost before either fix.

### Remaining cost, in order

| Rank | Item | Measured | Notes |
|---|---|---|---|
| **1** | Software rotation (§2.2) | **153 ms** | Now the single largest item |
| **2** | Base refresh blit, rotation excluded (§2.1/§2.3) | **131 ms** | Full-screen canvas→drawbuf→FB copy; the canvas is what forces the extra pass |
| 3 | Paint stage (grid/waypoints/geometry) | 32 ms | Only ~11 ms of this is draw calls |

> ⚠️ **Rank 2 above is wrong and is kept only as a record.** The 131 ms was
> `refr − rot`, i.e. everything not yet instrumented — not the canvas blit. When it was
> finally split (2026-07-28), the canvas blit turned out to be worth ~22 ms and the bulk
> was LVGL repainting two hidden full-screen backgrounds. See "The residual trap" below.

### Revised priority

| Rank | Item | Measured cost | Expected after fix | Effort |
|------|------|---------------|--------------------|--------|
| **1** | Replace `lv_canvas_fill_bg` with `lv_color_fill()` (or a 32-bit word fill) over `dsc->data` | 205 ms | ~5–11 ms | **XS** — a few lines |
| **2** | Refresh stage: full-screen 230,400 px blit canvas→draw buf→framebuffer | 131 ms | needs its own measurement | M |
| 3 | Everything else in this document | 10 ms total | — | — |

Item 1 alone should take the frame from **346 ms → ~150 ms**. Item 2 then becomes the whole
remaining cost and deserves the §2.1 "draw direct, delete the canvas" treatment — the canvas is
what forces the extra full-screen copy.

---

## ⚠️ THE RESIDUAL TRAP — read before estimating anything in this document

Three predictions in this file were wrong, all in the same way: **a residual was named after a
cause.** The pattern is worth stating explicitly because it survived two corrections.

| Claim | What was actually measured | Truth |
|---|---|---|
| "Software rotation is the single biggest item, 153 ms" (§2.2) | `refr` with nothing subtracted | Rotation was ~65 ms once isolated |
| "The canvas blit is 131 ms" (§2.1, rank 2 above) | `refr − rot` | Blit was ~22 ms |
| "Dropping the canvas saves ~104 ms" | `refr − rot − flush` | It saved ~28 ms |
| "Transpose tuning gets 64 → ~40 ms" | nothing — pure guess | It got 55.7 ms (C6) |

Each time the unmeasured remainder was given the name of whatever hypothesis was in play. Each time
the real cost was something nobody had thought to bracket. The fix was never cleverness — it was
adding one more timer and letting the residual shrink until it pointed at something specific.

**Rule for this document: never attribute a residual. If a number is `total − known`, call it
"unmeasured" and bracket it before proposing work against it.** The instrumentation is cheap
(`esp_timer_get_time()` either side of the suspect call); the rewrites justified by bad attribution
are not.

**Corollary, from C6: look for a bound before guessing.** The ~40 ms transpose estimate had no
measurement behind it at all, but a hard upper bound on what it could ever achieve was already in the
document — the flush moved the same 460 KB PSRAM→PSRAM with an optimized `memcpy` at 27 MB/s. Any
hand-written per-pixel loop over the same bytes was going to land under that. When estimating a
memory-bound rewrite, first find the cheapest existing operation that touches the same bytes and use
its measured throughput as the ceiling.

**Second corollary, from C7: constants tuned against a pipeline you are about to change are
provisional.** The transpose tile size was measured at 64 during step 9b and re-measured at 32 after
step 9 removed a competing memcpy — the 3.4 ms that justified the larger tile, and its 6 KB of SRAM,
had evaporated. Re-check tuning parameters after any change to what runs alongside them.

---

## ✅ MEASURED — 2026-07-28, after steps 7 + 8 + the cover-check fix

Full-frame refresh (230,400 px), TILED rotation, indoors:

```
--- label stage (updateRadarDisplay): 0.6 ms ---
--- paint stage (radarDrawEventCb — NESTED inside REFRESH) ---
  grid:            6170 us  (  6.2 ms)
  waypoints:       5115 us  (  5.1 ms)
  triangle+N+gauge:1765 us  (  1.8 ms)
  PAINT TOTAL:    13113 us  ( 13.1 ms)
--- refresh stage ---
  REFRESH:          149 ms   (230400 px)
    tiled rotate:           64108 us ( 64.1 ms)
    flush to framebuffer:   33899 us ( 33.9 ms)
    radar bg fill:          21452 us ( 21.5 ms)
    radar paint:            13113 us ( 13.1 ms)
    LVGL non-radar draw:    16428 us ( 16.4 ms)
FRAME TOTAL:      149.6 ms  (label 0.6 + refresh 149)
```

**Frame ~499 ms → 149 ms across the whole effort (~0.8 → ~6.7 fps).**

Note the timing semantics changed at step 8: the radar paints inside a draw event, so `paint` is a
component of `REFRESH`, not sequential with it. Frame = `label + refresh`. Adding `paint` to `refr`
double-counts, which is what the pre-step-8 FRAME TOTAL did — correctly, back when the canvas made
the two stages sequential.

### Where the 149 ms sat, and what steps 9b + 9 did to it

| Item | Was | Now | Note |
|---|---|---|---|
| Tiled rotate | 64.1 ms | **47.4 ms** | 9b tuning, plus 8 ms it gained for free when the flush stopped contending |
| Flush to framebuffer | 33.9 ms | **0.02 ms** | Deleted by the framebuffer swap |
| Radar bg fill | 21.5 ms | 21.5 ms | At PSRAM write bandwidth for 460 KB; little headroom |
| LVGL non-radar draw | 16.4 ms | ~23 ms | HUD widgets; varies with content, not touched by this work |
| Radar paint | 13.1 ms | 9.4 ms | grid could use rects instead of 3px lines (§3.1) |
| **FRAME** | **149.6 ms** | **94–101 ms** | ~6.7 → ~10 fps |

The projection was ~91 ms / ~11 fps, so this landed close — but the split was nothing like predicted:
9b returned less than half its estimate and 9 returned more than its own, because the two items were
not independent. See C6 and C7.

---

## ✅ MEASURED — 2026-07-28, after steps 9b + 9

Full-frame refresh (230,400 px), TILED rotation into the back framebuffer, indoors:

```
--- label stage (updateRadarDisplay): 0.3 ms ---
--- paint stage (radarDrawEventCb — NESTED inside REFRESH) ---
  grid:            4489 us  (  4.5 ms)
  waypoints:       3148 us  (  3.1 ms)
  triangle+N+gauge:1709 us  (  1.7 ms)
  PAINT TOTAL:     9365 us  (  9.4 ms)
--- refresh stage ---
  REFRESH:           94 ms   (230400 px)
    tiled rotate:           47359 us ( 47.4 ms)
    flush to framebuffer:      24 us (  0.0 ms)
    radar bg fill:          21475 us ( 21.5 ms)
    radar paint:             9365 us (  9.4 ms)
    LVGL non-radar draw:    23213 us ( 23.2 ms)
FRAME TOTAL:       94.3 ms  (label 0.3 + refresh 94)
```

**Frame ~499 ms → 94 ms across the whole effort (~0.8 → ~10 fps).**

### Where the 94 ms now sits

| Item | Cost | Share | Next move |
|---|---|---|---|
| Tiled rotate | 47.4 ms | 50% | ~16.5 MB/s against a ~27 MB/s memcpy ceiling. Remaining headroom is ~1.6× *at best* and needs hand-tuned Xtensa, not another loop rewrite |
| LVGL non-radar draw | 23.2 ms | 25% | HUD widgets — now the second-largest item and never yet investigated |
| Radar bg fill | 21.5 ms | 23% | 460 KB at PSRAM write bandwidth; little headroom |
| Radar paint | 9.4 ms | 10% | grid as rects instead of 3px lines (§3.1) is worth ~4 ms |

Note the two 460 KB full-screen writes that remain (rotate + bg fill) are **69 ms of the 94** and both
are near the memory ceiling. Further large wins have to come from doing fewer full-screen passes or
from raising the memory clock — i.e. the Tier 0 items (§1.1 CPU 240 MHz, §1.4 `bb_invalidate_cache`)
and step 10, not from more of what steps 7–9 did.

### Also corrected by the boot log

**§1.8 (flash DIO → QIO) is void.** The bootloader reports:

```
I (39) qio_mode: Enabling default flash chip QIO
I (40) boot.esp32s3: SPI Mode : QIO
```

Flash is **already running QIO** — `board_build.flash_mode = dio` in `platformio.ini` is overridden
at runtime by the bootloader's `qio_mode` step. Remove this item; there is nothing to gain.

---
**Scope**: Render pipeline, build configuration, task architecture.
**Goal**: Smooth navigation. Reduce per-frame cost so the radar can rotate/translate at 10–20 Hz.
**Progress**: ~1 Hz → **~10 Hz**. Rotation and translation both track the compass/GPS at 5 Hz, so
the **sensor rate is now the binding constraint** — the radar can render roughly twice as fast as it
is being asked to. §3.2's "raise the compass rate" is the highest-value item left for smoothness.

---

# ⬇ ORIGINAL ANALYSIS (2026-07-27) — HISTORICAL

**Everything from here down predates the measurements.** It is kept because the reasoning is useful
and because two of its confident conclusions were wrong in instructive ways. Where a section has
been implemented or disproved, it is marked. **Do not take costs from this half of the document** —
use the ✅ MEASURED sections above.

---

## 0. Read this first: what the actual problem is — ⚠️ HISTORICAL, largely superseded

> **Beware the number collision.** The "~149 ms per redraw" below is the *2026-07-27* figure for the
> old canvas pipeline. The *current* frame total is also ~149 ms, by coincidence. They describe
> completely different pipelines — the old one at ~1 Hz effective, the new one at ~6.7 Hz.
>
> Also superseded here: the four-PSRAM-passes table (stages 1–2 are gone with the canvas, stage 3 is
> now a tiled transpose at 64 ms not ~153 ms), and both consequences below — rotation is no longer
> pinned to 1 Hz (C2) and the mutex worst case is capped at one render per loop (§3.3).

The codebase contains its own measurement, in `src/utils/task_manager.cpp:148`:

> `each RADAR_REFRESH takes ~149ms × 8 queue items = 1.2s delay`

**~149 ms per radar redraw.** Two consequences follow, and together they are the "smoothness wall":

1. **The map only moves 1×/second.** `RADAR_REFRESH` is queued from the GPS block at
   `GPS_UPDATE_INTERVAL_MS = 1000` (`include/core/system_config.h:156`). Compass updates arrive at up
   to 5 Hz but `processUIUpdate()` **deliberately skips the redraw when GPS has a fix**
   (`src/utils/task_manager.cpp:673-682`) — precisely because a second redraw would cost another
   ~50–149 ms. So while walking with a fix, heading rotation updates at 1 Hz. Heading is the
   fast-moving quantity in handheld navigation; capping it at 1 Hz is what reads as "not smooth".

2. **The UI freezes for 149 ms per redraw.** `updateRadarDisplay()` runs inside `uiTask` **while
   holding `display_mutex`** (`src/utils/task_manager.cpp:154-176`). During that window
   `lv_timer_handler()` doesn't run, `device_manager::updateButton()` isn't polled, and touch isn't
   sampled. Button latency inherits the full redraw cost.

So: **the fix is not "redraw more often" — it is "make a redraw cheap"**, then raise the rate. Every
item below is ranked by how much it moves that 149 ms number.

### Where the 149 ms goes (derived from the code, needs confirming with a profiler — see §5)

One radar frame currently makes **four full-screen passes over PSRAM**, one of which is
cache-hostile:

| # | Stage | Where | Bytes touched |
|---|-------|-------|---------------|
| 1 | `lv_canvas_fill_bg` + grid + geometry into the 480×480 canvas | `navigation.cpp:945-1020` | 460 KB written |
| 2 | LVGL refresh blits the canvas image → LVGL draw buffer | LVGL `lv_draw_img` | 460 KB read + 460 KB written |
| 3 | **90° software rotation, in-place square transpose** | LVGL `draw_buf_rotate_90_sqr` | 460 KB read + 460 KB written, **~75 % of accesses are cache-line misses** |
| 4 | `esp_lcd_panel_draw_bitmap` memcpy into the panel framebuffer | `device_manager.cpp:818` | 460 KB read + 460 KB written |

Plus, continuously in the background, the bounce-buffer refill ISR copies the whole framebuffer out
of PSRAM every frame (§1.4).

Stage 3 is the prime suspect. `draw_buf_rotate_90_sqr` rotates four pixels at a time at four
addresses separated by a 960-byte stride. Three of those four accesses miss the 32 KB data cache on
essentially every iteration, so each 2-byte pixel costs a 32-byte line fill from PSRAM — a **16×
read amplification**. 230,400 pixels × 2 (read+write) × ~0.75 miss rate × 32 B ≈ **11 MB of real
PSRAM traffic for one rotation**.

Stage 3 has a second, independent cost. LVGL's own source says it (`lv_refr.c:1268`):

```c
/*FIXME: Rotation forces legacy behavior where rendering and flushing are done serially*/
while(draw_buf->flushing) {
    if(drv->wait_cb) drv->wait_cb(drv);
}
```

**Enabling `sw_rotate` disables double-buffered rendering.** The busy-wait after every chunk means
the second LVGL buffer (921 KB of PSRAM for the pair) buys nothing. LVGL also refuses to combine
rotation with `full_refresh` at all (`lv_refr.c:1181`), which is why `full_refresh = 0` is forced in
`device_manager.cpp:549`.

---

## 1. Tier 0 — Build configuration. Cheapest wins in the project.

These are `sdkconfig.defaults` edits. No application code changes. Do these first and re-measure
before touching architecture, because they change the baseline everything else is judged against.

### 1.1 The CPU is running at 160 MHz, not 240 MHz ⭐ biggest single free win

`sdkconfig.defaults` never sets the CPU frequency, so ESP-IDF's default applies. Verified in the
generated config:

```
.pio/build/cc-radar/config/sdkconfig.h:
#define CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_160 1
#define CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ 160
```

`CLAUDE.md` and `docs/` claim 240 MHz throughout (e.g. "MCU: ESP32-S3 @ 240MHz", "<2ms for 50
waypoints @ 240MHz"). **The docs are wrong; the binary runs at 160 MHz.** This is very likely a
leftover from the Arduino → ESP-IDF migration — the Arduino core sets 240 MHz by default, vanilla
IDF does not.

```ini
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y
```

**Confirmed against the board spec**: the ESP32-S3 carries a high-performance **Xtensa 32-bit LX7
dual-core** processor rated to a main frequency of **up to 240 MHz**. So 240 MHz is the part's rated
ceiling, not an overclock — this item is recovering the clock the silicon is specified for, and the
"240 MHz" figure in `CLAUDE.md` and `docs/` describes the hardware correctly even though the binary
does not currently run there.

- **Expected gain**: ~1.5× on every CPU-bound stage. On a redraw that is largely PSRAM-bound this
  will be less than 1.5× end to end, but it is free and it also shortens the bounce-buffer ISR.
- **Cost**: higher power draw — measure battery life impact, this is a handheld device.
- **Risk**: low. Watch for PSRAM timing at 240 MHz/80 MHz octal; it is a standard, well-tested combo.

### 1.2 Compiler is optimizing for size, not speed

```
#define CONFIG_COMPILER_OPTIMIZATION_SIZE 1        // -Os
#define CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_ENABLE 1
#define CONFIG_COMPILER_OPTIMIZATION_ASSERTION_LEVEL 2   // full assertions
```

LVGL's software renderer is exactly the kind of tight-loop code that `-Os` penalizes (no loop
unrolling, no aggressive inlining of the blend inner loops).

```ini
CONFIG_COMPILER_OPTIMIZATION_PERF=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_SILENT=y   # or _DISABLE once stable
```

- **Expected gain**: typically 10–25 % on LVGL blend/fill/rotate paths.
- **Cost**: binary grows ~5–10 %. Current `firmware.bin` is 1.66 MB in a 2 MB OTA slot → ~340 KB
  headroom. `-O2` should still fit, but **check the size after building** — an OTA slot overflow is
  a hard failure.
- **Risk**: low. Silencing assertions removes the `__FILE__` strings, which claws back some flash.

### 1.3 Caches are not at maximum

```
#define CONFIG_ESP32S3_INSTRUCTION_CACHE_16KB 1   // 32 KB available
#define CONFIG_ESP32S3_DATA_CACHE_32KB 1          // 64 KB available
```

All application and LVGL code executes from flash through the 16 KB instruction cache. The render
path touches a lot of distinct LVGL code (draw_rect, draw_line, masks, blend, canvas, refresh) and
will be thrashing it.

```ini
CONFIG_ESP32S3_INSTRUCTION_CACHE_32KB=y
CONFIG_ESP32S3_DATA_CACHE_64KB=y
```

- **Expected gain**: moderate on the geometry/draw stages; the icache bump is likely the more useful
  of the two.
- **Cost**: **cache is carved out of the 512 KB internal SRAM.** Going 16→32 KB icache and
  32→64 KB dcache costs ~48 KB of usable SRAM. Per `memory/sram_budget.md` static RAM is already at
  57.4 %. **Try one at a time and check the free-heap number after boot.** If SRAM is too tight,
  take the icache bump only.
- **Risk**: medium — this is the one Tier 0 item that can fail to boot or OOM later.

### 1.4 The bounce buffer is destroying the data cache every frame ⭐

`device_manager.cpp:425` enables a 10-line SRAM bounce buffer but leaves
`flags.bb_invalidate_cache` at 0.

In bounce-buffer mode the driver copies the framebuffer out of PSRAM into SRAM **through the data
cache**. That is 480×480×2 = 460 KB per frame at 37.7 fps = **~17.4 MB/s streamed through a 32 KB
cache**. The framebuffer is read exactly once and never reused, so every one of those lines is pure
pollution: it evicts the canvas and draw-buffer lines that the render code actually wants. The IDF
flag exists for exactly this:

> `bb_invalidate_cache`: If this flag is enabled, in bounce back mode we'll do a cache invalidate on
> the read data, freeing the cache.

```cpp
cfg.flags.bb_invalidate_cache = 1;
```

- **Expected gain**: potentially large and hard to predict — it directly attacks the cache-miss rate
  that dominates stage 3. Cheap to try.
- **Risk**: low. Read the IDF caveat about not having other CPU writes to the framebuffer in flight.

### 1.5 The bounce-buffer ISR is probably running on Core 1, competing with the UI task ⭐

`esp_intr_alloc()` binds an interrupt to **whichever core calls it**. `esp_lcd_new_rgb_panel()` is
called from the boot path, and `sdkconfig.defaults` sets:

```ini
CONFIG_ESP_MAIN_TASK_AFFINITY_CPU1=y
```

So the RGB panel's DMA/bounce ISR is very likely installed on **Core 1 — the same core as
`uiTask`** (`TaskConfig::UI_CORE = 1`). That ISR performs the ~17.4 MB/s PSRAM→SRAM memcpy from
§1.4, in interrupt context, stealing cycles from the render loop 48 times per frame.

- **Suggested change**: create the RGB panel from a short-lived task pinned to Core 0 (or move the
  whole display init there), so the ISR lands on Core 0 alongside the I2C/Network/System tasks.
- **Verify first**: print `xPortGetCoreID()` from `on_vsync_cb` — it runs in ISR context on the core
  the interrupt was allocated on. That single line confirms or kills this hypothesis.
- **Expected gain**: meaningful if confirmed — it removes a continuous ISR load from the render core.
- **Risk**: low-medium. Core 0 also runs the WiFi/BLE stack; watch for interaction during WiFi mode
  (radar is disabled in WiFi mode anyway, per `navigation.cpp:110`).

### 1.6 `LV_ATTRIBUTE_FAST_MEM` is unset — LVGL's hot functions run from flash

`include/ui/lv_conf.h` is minimal and never defines `LV_ATTRIBUTE_FAST_MEM`, so it defaults to
empty. LVGL explicitly tags its hottest functions with it — including `draw_buf_rotate_90`,
`lv_draw_sw_blend_basic`, `fill_normal`, and `map_normal`.

```c
#define LV_ATTRIBUTE_FAST_MEM IRAM_ATTR
```

- **Expected gain**: removes instruction-cache misses from the innermost pixel loops.
- **Cost**: pulls several KB of LVGL into IRAM. Check the IRAM figure in the link report; combined
  with §1.3 this is where the SRAM budget gets tight.
- **Note**: `draw_buf_rotate_90_sqr` — the function actually used for full-screen rotation — is
  **not** tagged, so this does not help the dominant path. Worth doing anyway, and it becomes more
  valuable once §2 removes the square-rotate path.

### 1.7 `LV_MEMCPY_MEMSET_STD` is off

Not defined in `lv_conf.h` → defaults to 0, so LVGL uses its own `lv_memcpy`/`lv_memset` instead of
newlib's. On Xtensa the ROM/newlib versions are hand-optimized.

```c
#define LV_MEMCPY_MEMSET_STD 1
```

- **Expected gain**: small but free; helps the fill and blit stages.

### 1.8 Flash is running in DIO, not QIO — half the flash read bandwidth ⭐

`sdkconfig.defaults` asks for QIO:

```ini
CONFIG_ESPTOOLPY_FLASHMODE_QIO=y
```

…but `platformio.ini` overrides it:

```ini
board_build.flash_mode = dio
```

and the override wins — the actual build output confirms it:

```json
.pio/build/cc-radar/flasher_args.json:
"write_flash_args": ["--flash_mode", "dio", "--flash_size", "16MB", "--flash_freq", "80m"]
```

**DIO transfers 2 bits per clock; QIO transfers 4.** All application and LVGL code executes from
flash through a 16 KB instruction cache (§1.3), so every icache miss in the render loop is being
served at half the possible rate. This looks like another Arduino → ESP-IDF migration casualty —
`CLAUDE.md` itself records the old Arduino setting as `board_build.arduino.memory_type = qio_opi`,
i.e. **QIO** flash + octal PSRAM. The current build silently regressed to DIO.

```ini
board_build.flash_mode = qio
```

- **Expected gain**: meaningful, and it compounds with §1.3 (bigger icache) and §1.6 (`FAST_MEM`) —
  all three attack the same "code is stalling on flash" problem from different sides.
- **Risk**: low but **verify on hardware**. QIO needs GPIO 8/9 free for flash IO2/IO3; on a
  WROOM-1-N16R8 module they are internal to the package, which is why `qio_opi` was the Arduino
  default for this board. If the board doesn't boot after the change, revert — the failure is
  immediate and obvious, not subtle.
- **Also check**: the two files disagreeing is a bug in itself. Pick one place to set flash mode.

### 1.9 LVGL is asked to refresh at 100 Hz on a 37.7 Hz panel

`lv_conf.h`: `#define LV_DISP_DEF_REFR_PERIOD 10`. The panel refreshes at
`10 MHz / (528 × 502) = 37.7 Hz`. Rendering more often than the panel can show is pure waste; every
extra pass still pays the rotation cost. Set it to the panel period (26 ms) — or leave it and rely
on the vsync gate, but then the 10 ms value is misleading and should be commented.

---

## 2. Tier 1 — Pipeline architecture. This is where the 149 ms actually lives.

### 2.1 Delete the radar canvas; draw in an `LV_EVENT_DRAW_MAIN` handler — ✅ DONE (C4, `816b421`)

> Implemented as described. **The gain estimate below was wrong** — "removes one of the four
> full-screen passes" was right, but that pass cost ~22 ms, not the ~104 ms this section's framing
> implied. Frame went 238 → 210 ms. The 460 KB PSRAM saving was accurate. The per-draw-call
> overhead concern was also real but small. See C4 and "The residual trap".


The radar is drawn into a full-screen `lv_canvas` (`ui_manager.cpp:190-216`, 460 KB of PSRAM), which
LVGL then treats as an image and **blits in full into the draw buffer** on every refresh. The canvas
is an intermediate that buys nothing: all of its content is code-generated geometry that could be
emitted straight into LVGL's real draw context.

Replacing it with a plain `lv_obj_t` plus a `LV_EVENT_DRAW_MAIN` event callback that calls the same
`lv_draw_rect` / `lv_draw_line` / `lv_draw_arc` functions against `lv_event_get_draw_ctx(e)`
eliminates:

- **Stage 2 entirely** — 460 KB read + 460 KB write per frame.
- **460 KB of PSRAM** permanently.
- **Per-draw-call overhead.** Every `lv_canvas_draw_*` call builds and tears down a fake display:
  `lv_disp_drv_init()` + `lv_mem_alloc(sizeof(lv_draw_sw_ctx_t))` + `lv_draw_sw_init_ctx()` +
  `lv_obj_invalidate(canvas)` (verified in `lv_canvas.c`, e.g. `lv_canvas_draw_rect`). At 1 km zoom
  a frame issues ~22 grid lines + triangle + up to 50 waypoints + 8 indicators + north indicator ≈
  **80+ of these setups and 80+ full-object invalidations per frame.**

- **Expected gain**: large — removes one of the four full-screen PSRAM passes plus all per-call
  overhead.
- **Effort**: medium. The drawing functions in `navigation.cpp` take `lv_obj_t* canvas`; they'd take
  a `lv_draw_ctx_t*` and use `lv_draw_*` instead of `lv_canvas_draw_*`. Coordinates become
  screen-absolute rather than canvas-relative (the canvas is already at 0,0 full-screen, so this is
  close to a no-op). The background fill becomes the object's `bg_color` style.
- **Risk**: medium — it is the largest single change here, but it's mechanical and testable
  incrementally.

### 2.2 Kill LVGL software rotation — ✅ DONE via Option A (C3, `ff82116`)

> Option A implemented: rotation 162 → 64.3 ms. **Option B is impossible** — LVGL's touch transform
> keys off `rotated` alone and assumes the pixels were rotated to match, so drawing pre-rotated
> leaves the UI visible but untouchable. **Option C is ruled out by the enclosure**: the panel is
> rotated specifically to move the GPS module off the bottom edge, where the enclosure indent and
> sky visibility require it. Neither is available; do not re-propose them.


`disp_drv.sw_rotate = 1` / `LV_DISP_ROT_90` (`device_manager.cpp:552-556`) costs, per frame: a
cache-hostile in-place 480×480 transpose (~11 MB of effective PSRAM traffic, see §0), **and** the
forced serial render→flush that nullifies double buffering, **and** the inability to use
`full_refresh`.

The physical reason it exists is that the panel is mounted 90° CCW in the enclosure. ST7701 in RGB
mode cannot do a 90° swap in hardware (MADCTL `MV` needs a line buffer the RGB path doesn't have),
so 180° is achievable in hardware but 90° is not. That leaves three real options:

**Option A — blocked (tiled) transpose in the flush callback.** Set `sw_rotate = 0`, and do the
rotation yourself in `lvgl_flush_cb` in 16×16 or 32×32 tiles into the panel framebuffer. A tiled
transpose keeps both source and destination tiles resident in cache, turning ~75 % misses into
near-full cache-line utilization — the same pixel count with roughly **an order of magnitude less
PSRAM traffic**. It also removes LVGL's forced `while(draw_buf->flushing)` busy-wait, restoring
double-buffered rendering.
- **Gain**: very large. **Effort**: small-to-medium — one self-contained function.
- **Combine with**: `num_fbs = 2` (below) so you transpose into a back buffer and swap, avoiding
  tearing.
- **This is the highest value-per-effort item in the document.**

**Option B — pre-rotate the drawing.** The radar screen's content is entirely code-generated
geometry; rotating it 90° in `latLonToScreen()`/`rotatePoint()` is free (add 90° to the heading
term). But the HUD labels, settings screens, and waypoint list are LVGL widgets whose *text* would
render sideways, and LVGL 8.x has no rotated-label support. Only viable if the UI is redesigned to
put all text in pre-rotated image assets. **Not recommended.**

**Option C — fix it in the enclosure.** Mount the panel in its native orientation. Zero software
cost, forever. Worth pricing against the engineering time for Option A.

### 2.3 Use the panel's own framebuffers as LVGL's draw buffers (`num_fbs = 2`) — ⭐ NEXT, unblocked

> **Prerequisites are now satisfied**: `sw_rotate` is off (C3) and the canvas is gone (C4). This is
> the top remaining item, worth the measured **33.9 ms** the flush currently costs.
>
> **Adjusted plan**: rotation still has to happen somewhere, so `full_refresh` + `direct_mode` as
> written below is not quite the shape. Instead, have `rotate90_tiled()` write straight into the
> back framebuffer from `esp_lcd_rgb_panel_get_frame_buffer()` and swap — that deletes both the
> `esp_lcd_panel_draw_bitmap` memcpy *and* the staging-buffer write, in one change to code we own.


Confirmed available in this IDF version
(`components/esp_lcd/rgb/include/esp_lcd_panel_rgb.h:151`, max 3 buffers). Currently `num_fbs` is
unset → 1 framebuffer, and the flush is a full memcpy into it (stage 4).

With `num_fbs = 2`, `esp_lcd_rgb_panel_get_frame_buffer()`, LVGL `direct_mode = 1` and
`full_refresh = 1`, LVGL renders **directly into the back framebuffer** and the flush becomes a
pointer swap at vsync — **zero copy**. That removes stage 4's 460 KB read + 460 KB write and the
two separate 921 KB LVGL buffers.

- **Prerequisite**: rotation must be gone first (§2.2) — `full_refresh` + `sw_rotate` is rejected by
  LVGL outright.
- **Expected gain**: removes the last redundant full-screen pass.
- **Cost**: +460 KB PSRAM for the second framebuffer, offset by −921 KB from dropping the separate
  LVGL buffers and −460 KB from dropping the canvas (§2.1). **Net PSRAM saving of roughly 900 KB.**

### 2.4 Re-test a higher pixel clock now that the bounce buffer exists ⭐

`PCLK_HZ = 10000000` is marked "proven stable (critical)" and `CLAUDE.md` records that 12 MHz caused
screen jitter. But `CLAUDE.md` **also** still says:

> "**Hardware Limitation**: ESP-IDF version doesn't support bounce buffer - large buffers are best
> alternative"

…while `device_manager.cpp:425` now *does* configure a 10-line bounce buffer. **The 10 MHz limit was
established before the bounce buffer existed.** RGB-panel jitter at higher PCLK is caused by the
LCD FIFO starving when PSRAM bandwidth is contended — which is precisely what a bounce buffer fixes,
by decoupling the panel's real-time fetch from PSRAM.

- **Suggested**: retest 12 / 14 / 16 MHz. At 16 MHz the panel runs at **60.3 Hz** instead of 37.7 Hz.
- **Also enable**: `CONFIG_LCD_RGB_RESTART_IN_VSYNC=y` so a transient underrun re-syncs at the next
  frame instead of leaving a permanent horizontal offset.
- **Consider**: raising `BOUNCE_BUFFER_LINES` from 10 to 20 (+18.75 KB SRAM) for more headroom.
- **Risk**: this is the item most likely to reintroduce visual artifacts. Test it in isolation,
  and keep 10 MHz as the documented fallback.
- **Caveat**: raising PCLK also raises the bounce-ISR memcpy rate proportionally (§1.4/§1.5), so do
  §1.4 and §1.5 first.

---

## 3. Tier 2 — Draw-level and scheduling work

### 3.1 Grid lines: use rects, not `lv_draw_line` with `width = 3` — still open, now measured

> **Updated**: the calls are `lv_draw_line` now, not `lv_canvas_draw_line` (C4). Measured cost is
> **6–9 ms**, not the 4.7 ms this section assumed — and it was 20–26 ms until the `clip_corner` fix
> (C5) removed the radius mask every line was blending through. Still the largest single item in the
> 13 ms paint stage, so the argument below holds; the payoff is just smaller than the effort table
> once suggested.

`drawRadarGrid()` draws up to 22 axis-aligned lines per frame at `line_dsc.width = 3`. LVGL's thick-line path builds anti-aliasing masks and blends through the mask
pipeline — dramatically more expensive than a solid fill.

Every one of these lines is axis-aligned, so each is just a 3×480 or 480×3 rectangle. A
`lv_draw_rect` with `bg_opa = LV_OPA_COVER`, `radius = 0`, no border and no shadow hits LVGL's
`fill_normal` fast path (a per-row `lv_color_fill`), skipping masks entirely.

- **Expected gain**: solid. 22 mask-based line draws → 22 memset-class fills.
- **Effort**: small. **Risk**: very low (visually near-identical; you lose AA on lines that are
  axis-aligned and therefore have nothing to anti-alias).

### 3.2 Decouple heading rotation from the 1 Hz GPS redraw — ✅ DONE (C2)

The suppression is removed and compass updates now drive redraws via `requestRadarRender()` /
`flushRadarRender()` coalescing. Rotation is at 5 Hz with a fix. See C1/C2 at the top.

Remaining sub-items, **not** done, in order of likely value:

- **Raise the compass rate — now the top item, and the situation has inverted.** At 149 ms the radar
  renders at ~6.7 Hz while only being *asked* to render at 5 Hz, so the sensor rate is finally the
  binding constraint rather than frame cost. The read is gated to 20 ms but the System Task loop is
  `SYSTEM_UPDATE_MS = 200` → effective 5 Hz; splitting the compass into its own task, or dropping
  the System Task period, gets 10–20 Hz. The coalescing in C2 makes this safe — a faster producer
  cannot multiply renders per UI loop.
- Reconsider the 1.5° deadband (`task_manager.cpp`). It exists to cut render load; with a cheap
  frame it becomes the thing standing between you and smooth rotation. Lower it to ~0.5° and lean on
  `smoothHeading()`'s EMA instead.
- ~~Interpolate GPS position between the 1 Hz fixes~~ — **not needed.** `GPS_UPDATE_INTERVAL_MS` went
  1000 → 100 in `d289707`; the BH-880 emits NAV-PVT at a fixed 10 Hz regardless of what it is asked
  for, so translation already matches rotation at 5 Hz with no dead reckoning.

**Note**: `memory/compass_architecture.md` and `CLAUDE.md` describe the compass as ~1 Hz; the code
now reads at up to 5 Hz. Worth reconciling.

### 3.3 Don't hold `display_mutex` across the whole redraw — ✅ RESOLVED by C2

Was: `uiTask` takes `display_mutex`, then runs `lv_timer_handler()` **and** up to 4 queued updates —
each of which may be a full `updateRadarDisplay()`. Worst case 4 × 149 ms = ~600 ms with the mutex
held and no button polling.

The render-coalescing in C2 caps renders at **one per loop iteration** regardless of batch size,
which is the "process one render-class update per loop" option this item proposed. Non-render queue
items (battery, screen loads, beacon dBm) are still processed up to 4 per loop, but they are cheap.

### 3.4 The vsync gate doesn't actually pace anything

```c
xSemaphoreTake(vsync_sem, pdMS_TO_TICKS(30));
```

`g_vsync_sem` is a **binary** semaphore given from the ISR every 26.6 ms. If a loop iteration
overruns one frame period (which a 149 ms redraw always does), the semaphore is already signalled
and the take returns immediately — so the loop spins with no delay at all until it catches up. It
works as intended only once frames are under one panel period. Worth a comment, or a counting
semaphore, so the intent survives.

### 3.5 `Serial` flushes on every call

`SerialClass::printf` and every `print` overload end in `fflush(stdout)`
(`src/core/arduino_compat.cpp:25-71`). With `CONFIG_ESP_CONSOLE_USB_CDC`, each flush is a write into
the TinyUSB CDC path and can stall when no host is draining it. There are 94 `Serial.*` calls in
`navigation.cpp` + `task_manager.cpp` alone, several on interaction paths (zoom change, screen
transitions).

- **Suggested**: add a global "logging enabled" gate that early-returns before formatting, and drop
  the unconditional `fflush` (stdout is line-buffered already).
- Also note `SerialClass::available()` calls `fgetc(stdin)` — it consumes a byte to test for one.
  It works because of the ring buffer behind it, but it's fragile and does a VFS call per poll.

### 3.6 Recompute less per frame

Minor next to the above, but nearly free:

- `rotatePoint()` calls `cos()`/`sin()` (double-precision) **per waypoint per frame** with the same
  angle. Hoist `cos_a`/`sin_a` out of the loop and use `cosf`/`sinf`.
- `latLonToScreen()` does a full double-precision Haversine plus `atan2`, `sqrt`, four `sin/cos` per
  call. `drawWaypoints()` already hoists `cos_lat1`/`sin_lat1` — `handleTapAt()` does not, and calls
  `latLonToScreen()` per waypoint on every tap.
- At radar scales (≤ 1 km) the equirectangular approximation
  (`dx = R·Δlon·cos(lat)`, `dy = R·Δlat`) is accurate to well under a pixel and costs two multiplies.
  Haversine is overkill here.
- `getColorScheme()` is called 4–6 times per frame; each call re-checks `millis()` and reaches into
  settings. Fetch it once per `updateRadarDisplay()` and pass it down.

---

## 4. Tier 3 — Hygiene (no runtime impact, but it's why the walls felt like walls)

### 4.1 The documentation describes a different system than the one that builds

This matters more than it looks: several of the findings above are cases where a documented
"hardware limit" was really a stale note. Concrete drift:

| Doc claim | Reality |
|---|---|
| `CLAUDE.md`: "MCU: ESP32-S3 @ 240MHz" | Builds at **160 MHz** (§1.1) |
| `CLAUDE.md`: "framework = arduino", `[env:cc-moat-port]` | `platformio.ini` is `framework = espidf`, `[env:cc-radar]` |
| `CLAUDE.md`: "ESP-IDF version doesn't support bounce buffer" | Bounce buffer is configured and active (§2.4) |
| `CLAUDE.md`: "40-line bounce buffer", "BUFFER_LINES 40/50/120/160" | `BUFFER_LINES = 480` |
| `CLAUDE.md`: "Use full refresh for stability / `full_refresh = 1`" | Code sets `full_refresh = 0` |
| `CLAUDE.md`: partitions "3MB app + 10MB FFat" | Build uses `partitions_ota.csv` — 2×2 MB OTA + 11.7 MB FFat |
| `CLAUDE.md` / memory: compass "~1 Hz" | Read gate is 20 ms, effective ~5 Hz |
| `CLAUDE.md`: GPS heading fusion, "NMEA RMC sentence fields 7-8" | Compass is the sole heading source; GPS is UBX |
| Docs: "<2ms for 50 waypoints @ 240MHz" | Waypoint drawing is ~5 ms; the *full frame* is ~149 ms @ 160 MHz |

**Partly reconciled 2026-07-28**: `CLAUDE.md` gained a Render Pipeline section covering the current
architecture (no canvas, tiled transpose, the two load-bearing style constraints) and the 240 MHz
row is now understood — the hardware genuinely is rated to 240 MHz (§1.1), only the build config
disagrees. The remaining rows are still drift and still worth a pass.

### 4.2 Dead weight

- `src/hardware/sensors/gyro_qmi8658.cpp` (305 lines) — documented as "unused, hardware on board".
- Four stale build environments in `.pio/libdeps/` (`cc-moat-port`, `cc-radar-compass`,
  `sd_card_coexistence`, `waveshare-esp32-s3-lcd-2_1`), each with a full LVGL copy. Disk only, but
  they make grep/tooling noisy and can confuse a rebuild.
- `partitions/partitions.csv` is no longer referenced by `platformio.ini` (which points at
  `partitions_ota.csv`); it's still shipped and describes a layout the firmware doesn't use.
- `lv_conf.h` disables nothing. LVGL defaults compile in every widget, every theme and every layout
  engine. Flash-size only, but §1.2 will make flash tighter — this is where to claw it back.

---

## 5. Do this before any of it: measure — ✅ DONE, and it was the whole ballgame

> This section was right about everything that mattered. Every wrong estimate in this document
> comes from a section that was written *without* the corresponding measurement, and every one was
> corrected by adding one timer. The `perf` command and DEV HUD (`8d2ac29`) now report the full
> per-stage split; `flush_us` and the `DRAW_MAIN_BEGIN` bracket were added later as the residual
> shrank. Item 3 below needs one correction: `ROTATION_DEGREES = 0` does not just make the UI
> sideways, it makes it **untouchable** (LVGL's input transform assumes the pixels were rotated), so
> it is a measurement mode only — use `rot on|off|tiled` at runtime instead of a rebuild.

The 149 ms is a single number from a comment. Everything above is inference from the source. Before
spending effort, spend an hour getting a breakdown — otherwise you risk optimizing stage 1 when 80 %
of the time is in stage 3.

1. **Turn on what's already there.** `LV_USE_PERF_MONITOR 1` is already set in `lv_conf.h` — the FPS
   / CPU overlay is on screen right now. Record the baseline number.
2. **Bracket the stages.** `esp_timer_get_time()` around: (a) the canvas draw block in
   `updateRadarDisplay()`, (b) `lv_timer_handler()`, (c) the body of `lvgl_flush_cb`. Accumulate and
   print once a second. The gap between (a) and (b) is stages 2+3.
3. **Isolate the rotation.** Set `ROTATION_DEGREES = 0` for one build and read the FPS monitor. The
   UI will be sideways and unusable — that's fine, it's a five-minute measurement that tells you
   exactly what §2.2 is worth.
4. **Confirm the ISR core.** One `xPortGetCoreID()` printf from `on_vsync_cb` settles §1.5.
5. **Use the profiler.** `CONFIG_APPTRACE_*` / `esp_gcov`, or simply
   `vTaskGetRunTimeStats()` with `CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS=y`, to see how CPU splits
   across `uiTask`, the idle tasks, and interrupts on each core.

---

## 6. Suggested order of work

| Step | Item | Effort | Expected impact | Risk | Status |
|---|---|---|---|---|---|
| 0 | Instrument (§5) | S | — | — | ✅ done (`perf` / DEV HUD, `8d2ac29`) |
| — | **`fill_bg` → `lv_color_fill` (C1)** | XS | **9.6× on the dominant call** | Low | ✅ done `fa63a03` |
| — | **Decouple heading redraw (§3.2 → C2)** | XS | **1 Hz → 5 Hz rotation** | Low | ✅ done |
| — | Mutex worst case (§3.3) | — | — | — | ✅ resolved by C2 |
| 1 | CPU 240 MHz (§1.1) | XS | **High** | Low (power) | open |
| 2 | `bb_invalidate_cache` (§1.4) | XS | **High** | Low | open |
| 3 | Flash mode DIO → QIO (§1.8) | XS | — | — | ❌ void — already QIO |
| 3b | `-O2` + silent assertions (§1.2) | XS | Medium | Low (flash) | open |
| 4 | Confirm + move panel ISR to Core 0 (§1.5) | S | Medium | Low | open |
| 5 | Grid lines as rects (§3.1) | S | Low (4.7 ms total) | Very low | open |
| 6 | Cache sizes, `FAST_MEM`, `MEMCPY_STD` (§1.3/1.6/1.7) | S | Medium | Medium (SRAM) | open |
| 7 | **Tiled transpose, drop `sw_rotate` (§2.2 A → C3)** | M | **162 → 64.3 ms rotation** | Medium | ✅ done `ff82116` |
| 8 | **Drop the canvas → `DRAW_MAIN` (§2.1 → C4)** | M–L | 238 → 210 ms, −460 KB PSRAM | Medium | ✅ done `816b421` |
| 8b | **`clip_corner` cover-check fix (C5)** | XS | **210 → 149 ms** | Low | ✅ done `44f6d0d` |
| 9b | **Transpose tuning: `IRAM_ATTR` + SRAM scratch tile (→ C6)** | S | 64.1 → 55.7 ms rotation | Low | ✅ done `311ca3c` |
| 9 | **`num_fbs = 2`, transpose into the back FB (§2.3 → C7)** | M | **flush 34 → 0.02 ms, frame 145 → 94 ms** | Medium | ✅ done `311ca3c` |
| 10 | Re-test higher PCLK (§2.4) | S | High (60 Hz) | **Medium-high** | open |
| 11 | Waypoint memory (see ROADMAP) | M | Raises the 50-waypoint cap | Low | open |
| 12 | Serial flush / recompute-per-frame (§3.5–3.6) | S | Low–medium | Low | open |
| 13 | Doc reconciliation (§4.1) | S | — | — | open |

**What the completed work changed about this plan.** The original ordering assumed smoothness had to
be bought with steps 7–10 first, and listed the heading decouple last as the thing that finally cashes
it in. In practice C1 + C2 — both XS — delivered the felt result on their own, because the second
problem was never frame *cost*, it was frame *rate*: the redraw was being skipped, not running slow.

**Effort did not track impact.** Ranked by measured ms per unit of work, the order was almost the
inverse of this table's original estimates:

| Change | Effort | Measured |
|---|---|---|
| `clip_corner` fix (C5) | XS — one style flag | **−61 ms** |
| `fill_bg` → `lv_color_fill` (C1) | XS — a few lines | **−184 ms** |
| Tiled transpose (C3) | M — one function | −98 ms |
| `num_fbs = 2` (C7) | M — config + flush path | −51 ms |
| Drop the canvas (C4) | M–L — 62 call sites | −28 ms |
| Transpose tuning (C6) | S — one function | −8 ms |

The two cheapest changes delivered the most. Both were found by measurement, not by reading code and
reasoning about it — and C4, the largest rewrite in the list, returned the least. Bracket first.

**Frame ~499 ms → 94 ms (~0.8 → ~10 fps).** What remains is 69 ms of full-screen PSRAM writes
(rotate 47.4 + bg fill 21.5) sitting near the memory ceiling, so the next real wins are the untried
Tier 0 items and step 10 — raising the clocks — not more pipeline surgery.
