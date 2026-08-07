# Performance Optimization Backlog

**Status**: Render work largely implemented. **Frame ~499 ms → 85.2 ms (~0.8 → ~11.7 fps).** See
"Completed" below for what shipped and the measured breakdown for where the remaining 85 ms sits.
Sections 0–5 are the **original 2026-07-27 analysis, preserved as written** — several of their
conclusions were disproved by measurement, and the ones that were are flagged inline. Trust the ✅
sections over them.

**§7 and §8 are not about the render.** They were added 2026-07-31 after the question "did anyone
check anything other than the render?" — the answer was no; this document had been scoped to a single
number, frame time. §7 covers **beacon proximity** (rate-starved at 2 Hz against a 5 Hz source); §8 is
a **full audit of every remaining subsystem** — sonar/buzzer, input latency, GPS, compass, battery.
**The highest-value open work in this document is now in those two sections, not in §1–§6.** Unlike
§1–§6 none of it is measured on hardware yet; the confidence of each claim is stated inline.

**Date**: 2026-07-27 (measurements + completions: 2026-07-28; §7 + §8 added 2026-07-31; work queue
merged in from the former `docs/TODO_next_session.md` 2026-07-31)

---

## 🔭 WORK QUEUE — what to actually do next

**This is the only live to-do list for this effort.** `docs/TODO_next_session.md` was a second,
parallel copy of it and has been deleted — everything it tracked is either here or already in §7/§8.
Sections below this one are the analysis and the record; this one is the plan.

**Last verified state**: builds clean at **RAM 167,840 B / 327,680 B (51.2%), flash 1,706,415 B /
4,194,304 B (40.7%)** — 2026-08-07, after the OTA-partition grow to 4MB slots ([ADR-0024](adr/0024-ota-partitions-grown-from-unused-ffat.md)); the flash % dropped from the low-70s figure this
line used to carry for that reason, not because anything shrank. On-device, radar + beacon discovery
+ sound + button all confirmed working after the `I2C_PROCESS_MS` revert. Frame ~85 ms against a
10 Hz sensor feed.

### Standing rules

- **Don't commit until the change is verified on hardware** (docs-only commits excepted).
- **Measure build impact (RAM/flash) on every code change** — stash-build-restore for the baseline.
- **Never attribute an un-instrumented residual to a hypothesis.** Bracket it with
  `esp_timer_get_time()` first. See "The residual trap".
- **Read the IDF/library source before implementing anything that rests on a claim about its
  behaviour** — `grep -rn "<thing>" ~/.platformio/packages/framework-espidf/components/`. **Four**
  items in this document were void or misattributed that way.
- **A verified symptom and an unverified cause are different things** — §8.1b's stale root cause is
  the standing example. Don't write them in the same voice, and don't build on the second one.

### 0. Do this first: one field session, no code

Five changes are built, healthy in the build, and **unverified on hardware**. They are stacked on
each other, so anything further built before this session lands on an unverified base.

- [ ] **§8.1e** — walk a fixed waypoint in from ~50 m at 50 m zoom. Tempo should *glide* with no
      steps, be noticeably calmer beyond ~25 m, and go silent the moment you tap the waypoint within
      15 m. Check it re-engages if you unfix/refix.
- [ ] **§7.5 priority release** — fix a waypoint, walk into beacon range, confirm the fix releases
      and the beacon takes the buzzer.
- [ ] **§7.5 choppy-sonar fix** — hunt the tag; the tempo should feel like a smooth glide and the
      beep-length change should read as a trend signal, not as noise.
- [ ] **§7.3a battery drain** — 100% scan duty is the one genuinely new cost, zoom-gated to 50 m. If
      it's objectionable, `SCAN_WINDOW_MS` is the single knob (80 ms → 80% duty).
- [ ] **§8.3 GPS chunked UART read** (added 2026-07-31) — fix acquired, sat count normal, heading
      tracks. Core 0 only, so a failure here can't be confused with the audio/beacon items above.
- [ ] **I2C bus watchdog** — nothing to trigger deliberately; watch for `[I2C] ... bus appears
      wedged, attempting recovery` and whether a freeze now self-clears in ~2-4 s. Retune the
      10 / 2000 ms / 5 constants **only** from real log data, never by re-deriving the reasoning that
      picked them.

⚠️ **Confirm what the tag's advertising interval is currently set to** before assuming 200 ms.

### 1. Open bugs

- [x] ✅ **Resolved 2026-08-05 — superseded, kept for the record.** The "recurring freeze = wedged
      I2C bus" root cause this item was chasing turned out to be a real ESP-IDF driver bug (an
      unbounded busy-wait after a NACK in `i2c_master.c`, matching upstream
      `espressif/esp-idf#17720`), not the two application-level theories investigated here at the
      time (the probe-loop artifact and the boot-ping/standby-wake coincidence — both real findings,
      neither the actual cause). Patched at build time, field-verified over ~10.5h combined runtime
      with zero freezes. See [`i2c_bus_freeze_investigation.md`](i2c_bus_freeze_investigation.md) and
      [ADR-0021](adr/0021-i2c-nack-hang-build-time-backport.md). *Why* the bus produces NACKs at all
      is still open, lower priority — see that doc's Open Questions section.
- [ ] **Do not diagnose a freeze from serial silence.** The USB console went dead for ~4 minutes on
      2026-07-31 while the device kept working perfectly — touch, zoom and beacon all fine. Silence
      on the host proves nothing about the firmware. Both a subagent and the main agent called it a
      "boot hang" before the device owner pointed out it was running.

- [ ] **DEV/perf HUD froze at stale values** after a self-recovered I2C freeze, while touch, button,
      sound and rotation all came back. The recorded hypothesis (dangling `ui.perf_label`) **looks
      weak**: the label is created once on the radar stage (`ui_manager.cpp:311`) and nothing deletes
      it — `lv_obj_del` appears only for the standby screen and the WiFi modals. If it recurs, try
      `dev off` / `dev on` first; that one command discriminates between the pointer theory and a
      render-path stall. Not root-caused.
- [ ] **§8.1b `I2C_PROCESS_MS` 10 ms failure is un-diagnosed.** The symptom and the revert are
      confirmed on hardware; the *cause* written down for it was stale (see §8.1b). Do not retry
      10 ms on the strength of "the `Wire` bypass is gone".

### 2. Code work, ranked

- [ ] **§1.4 / §1.5 bounce-buffer A/B** *(S, medium risk)* — one `BOUNCE_BUFFER_LINES = 0` build
      answers both items and frees **18.75 KB SRAM**. The safety blocker isn't one:
      `on_frame_buf_complete` fires in both modes (`esp_lcd_panel_rgb.c:871`). But the panel would
      then stream directly from PSRAM, competing with `rotate`. **Two builds + the `perf` HUD, be
      ready to revert** — risk lands on display stability, which is currently flawless. Give it its
      own session; do not fold it into the §0 field build, or a regression can't be attributed.
- [x] **§8.3 GPS bulk UART read** — ✅ **built 2026-07-31, awaiting hardware verification.** Now
      reads up to 256 bytes per `uart_read_bytes()` call instead of 1; ~100 syscalls per NAV-PVT
      frame become 1. UBX state machine untouched. +88 B flash, +256 B System Task stack, ±0 static
      RAM. **Verify**: still acquires a fix, sat count normal, heading tracks. Add to the next field
      session — it's independent of §0's audio/beacon checks, so a regression is still attributable.
- [x] **§3.6 recompute less per frame (Haversine → equirectangular)** — ✅ **done 2026-08-05**, and
      `MAX_WAYPOINTS` raised to 500 in the same change. See [ADR-0022](adr/0022-waypoint-cap-raised-to-500-not-700.md).
      `rotatePoint()`'s per-waypoint `cos`/`sin` hoist done 2026-08-07, see §3.6 below. `getColorScheme()`
      caching turned out to be a false lead — already gated behind a 1s `millis()` check, not a real cost.
- [ ] **§8.2 decouple touch polling** *(M)* — ~11.7 Hz, fine for taps, coarse for drags. **Weakest
      claim in the audit — justify by actual annoyance before paying for it.**
- [ ] **§8.1c buzzer EXIO read-modify-write** — 2 transactions per edge. Only worth it if the tick
      rate goes up, which §8.1b currently blocks.
- [x] **§4.1 / §4.2 hygiene** — ✅ **done 2026-07-31.** Comments and docs only, no behaviour change
      (+8 B flash, alignment noise). Covered: the 5 Hz→10 Hz GPS comments and `STABLE_SAMPLES`'
      silently-halved meaning, the void CDC-stall justification, the vsync gate that doesn't gate,
      `LV_DISP_DEF_REFR_PERIOD` (kept at 10 — LVGL source read first; it also retimes animations),
      `flash_mode = dio` (correct, annotated so nobody "fixes" it), the orphaned
      `partitions/partitions.csv`, and four false `CLAUDE.md` claims — Arduino build config, the
      superseded display section (bounce buffer / `full_refresh` / sw-rotation all inverted), GPS-NMEA
      heading fusion, and an unmeasured "<2 ms waypoints". Full list in CHANGELOG → Unreleased →
      Documentation. **Remaining §4.2 dead weight, not done**: `gyro_qmi8658.cpp` (305 unused lines),
      the four stale `.pio/libdeps/` envs, `lv_conf.h` disabling nothing.

### 3. Explicitly do NOT do

`-O2` (spends flash, the scarcest resource, to buy ms that can't be spent) · cache sizes (costs
48 KB SRAM) · higher PCLK (may be net-negative, §2.4) · grid-as-rects (§3.1 — paint measured 1.01× on
a 1.5× clock, so it is not compute-bound) · `bb_invalidate_cache` (**void**, no implementation in
IDF 5.5) · `I2C_PROCESS_MS` 10 ms (**void**, broke the device).

### 4. Verified healthy — don't re-audit

**Compass** (200 Hz ODR / 512× OSR, read at 10 Hz, EMA correctly re-derived 0.8 → 0.3 when the rate
changed — the example the others should follow) · **battery** (15-sample averaging, 30 s history) ·
**button** (~85 ms worst case; a real press is 132–186 ms) · **render** (85.2 ms/frame outruns the
10 Hz sensor feed; four flags are load-bearing, see CLAUDE.md).

### 5. Out of scope for this queue

**Beacon direction finding** is an *experiment*, not an optimization — feasibility risk on something
unproven, where everything else here is regression risk on something that works. It is unblocked
(4.24–4.37 Hz measured live) and tracked under **Planned** in [`../ROADMAP.md`](../ROADMAP.md), with
the design in [`beacon_direction_finding.md`](beacon_direction_finding.md). Opt into it explicitly;
don't fold it into a performance pass.

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

### Still open — but read this before picking anything up

**The render is no longer the bottleneck, and nothing below is needed to make the device work well.**
At 85.2 ms/frame (~11.7 fps capability) against a 10 Hz sensor rate, the pipeline now outruns what
feeds it. Verified outdoors with satellites locked: rotation, buttons and touch all good. Everything
remaining is optional.

**The best remaining work is not in the render at all — it is §7, beacon proximity.** That subsystem
has the same defect the render had before C2 (rate-starved, not compute-bound), it is still at 2 Hz
against a 5 Hz source, and unlike everything below it is a *felt* improvement rather than milliseconds.
Start there.

**Re-ranked 2026-07-31 against a different objective**: the remaining items were originally ordered by
*milliseconds saved*, but frame time is no longer scarce — 85 ms against a 10 Hz sensor feed. The
scarce resources are **flash (79.5 % of the OTA slot)** and **SRAM (59.7 %)**, and two of the items
below *consume* those to buy ms that cannot be spent. Ranked by resource-per-risk instead:

1. **§1.5 panel ISR core** — one `xPortGetCoreID()` printf in `on_vsync_cb`. Not a change, a
   *measurement*: it either confirms the ISR is stealing Core 1 from the UI task, or kills the
   hypothesis so the item can be deleted. Cheapest way to stop carrying an unknown.
2. **§3.5 `Serial` fflush gating** — argued on **reliability, not performance**. `SerialClass` ends
   every call in `fflush(stdout)`, which on the USB CDC path stalls unboundedly when the host isn't
   draining. That is System Task blocking, not "some ms", and it will bite hardest during beacon DF
   calibration when logging is heavy.
3. **§1.4 bounce-buffer A/B** — *not* the flag (void, see below), but removing the bounce buffer
   entirely to free **18.75 KB SRAM**. Measure both builds; be ready to revert, since the risk lands
   on display stability.
4. **§3.6** — per-frame recompute. Small, safe hygiene.

Void or actively counterproductive:

- **§1.4 `bb_invalidate_cache = 1` — ❌ VOID.** The flag has **no implementation** in IDF 5.5, and its
  premise contradicts the driver's deliberate `Cache_Start_DCache_Preload`. Full write-up in §1.4.
  It had been ranked #1 here.
- **§1.2 `-O2` + silent assertions** — spends the **scarcest** resource (flash, 79.5 % of a 2 MB OTA
  slot) to buy the **least scarce** (ms). Revisit only if a future feature actually needs Core 1 back.

Deprioritized or not worth it:

- **§2.4 higher PCLK — may make the frame *slower*.** See the 2026-07-29 re-assessment in that
  section: at 16 MHz the panel would read 27.8 MB/s from PSRAM against a bg fill that only achieves
  22.5 MB/s and is already bandwidth-bound. Higher PCLK buys a shorter tearing window, not fps.
- **§1.3 cache sizes** — costs ~48 KB of internal SRAM at 59.7% static already. Bad trade now.
- **§1.6/§1.7 `FAST_MEM` / `MEMCPY_STD`** — `FAST_MEM` was aimed at LVGL's `draw_buf_rotate_90`,
  which we no longer call at all (own tiled transpose). Marginal.
- **§3.1 grid lines as rects** — grid is 5.4 ms of a 9.3 ms paint stage, and paint measured 1.01× on
  a 1.5× clock, so it is not compute-bound. The payoff keeps shrinking.
- **§1.8 flash QIO** — void, and the reason is subtle: `flasher_args.json` says `dio`, but that only
  governs the ROM bootloader's initial load. `CONFIG_ESPTOOLPY_FLASHMODE_QIO=y` makes the 2nd-stage
  bootloader upgrade the chip at runtime, and the boot log confirms it: `qio_mode: Enabling default
  flash chip QIO` / `SPI Mode : QIO`. **Already QIO.** (The two config sites disagreeing is still
  worth tidying — §4.2.)

Rotation at 38.3 ms remains the largest single stage (~45% of the frame). The 240 MHz measurement
showed it had ~24% CPU headroom, so it is *not* purely bandwidth-bound as previously assumed — it is
the best target if anyone wants to keep going.

---

## 7. Beacon proximity — the same shape of problem, a different subsystem

**Added 2026-07-31.** Everything above this section is about the render pipeline; this section is the
answer to "did anyone check the *other* subsystems?" The answer was no — the whole effort was scoped
to one number, frame time. A read of the beacon path found a second subsystem with the same
structural defect the render had, and it is the only one. Compass and GPS are at 10 Hz and bounded by
the render; input polling is bounded by frame time and verified fine outdoors; battery sampling is
5 s and irrelevant; the buzzer's sonar timer is autonomous at 10 ms and already tight.

**The 240 MHz change buys the beacon nothing.** It was never compute-bound. Like the render before
C2, it is *rate*-starved: the pipeline is fed too rarely, and no amount of CPU fixes that.

### 7.1 The measurement: the feed is 2.0 Hz, the tag offers 5

Three things in `src/hardware/connectivity/beacon_proximity.cpp` combine into a hard ceiling of one
RSSI sample per 500 ms:

1. NimBLE defaults to **controller-side duplicate filtering** — `m_scan_params.filter_duplicates = 1`
   (`NimBLEScan.cpp:37`). `NimBLEScan.cpp:137` then suppresses every callback after the first for a
   given device within a scan session. One packet per device per scan, however many arrive.
2. `onResult` calls `g_pScan->stop()` (`:93`), deliberately ending the scan on the first hit.
3. The next scan is gated by `SCAN_INTERVAL_MS = 500` (`:18`).

| | effective samples/sec |
|---|---|
| today | **2.0** |
| continuous scan, tag at 200 ms (current tag setting) | 5 (~4 at the current 80 % scan duty) |
| continuous scan, tag at 100 ms (tag's fastest) | 10 (~8) |

**Tag advertising interval is 200 ms, configurable down to 100 ms** — user-confirmed 2026-07-31, so
unlike the render work this one did *not* need instrumenting first; the ceiling is a known quantity.
That is the only reason a number appears here without a bracket around it.

The 80 % is our own choice, not a limit: `setInterval(100)` / `setWindow(80)` (`:284-285`) are in
NimBLE's 0.625 ms units, i.e. **62.5 ms interval / 50 ms window**. Setting window == interval listens
continuously and captures ~every advertising event instead of ~4 in 5.

### 7.2 What the 2 Hz feed costs, in seconds

The smoothing constants were tuned against that starved feed and encode it implicitly:

- `EMA_ALPHA = 0.4` (`:21`) → 90 % settle at `ln(0.1)/ln(0.6)` = **4.51 samples** = **2.25 s** at 2 Hz.
- `ZONE_CHANGE_SAMPLES = 2` (`:31`) → **+1.0 s** before a zone change is confirmed.

**≈3.3 s from the user actually moving to the ring changing width.** That is the sluggishness, and it
is structurally the same finding as C2: the redraw wasn't slow, it was being skipped.

### 7.3 Three items

#### 7.3a Continuous passive scan — the 2.5× ⭐

Replace the stop/restart cycle with a scan that never stops:

```cpp
g_pScan->setActiveScan(false);       // passive — see 7.3d, we only need MAC + RSSI
g_pScan->setDuplicateFilter(false);  // controller stops deduping
g_pScan->setMaxResults(0);           // callbacks only; library erases each device after
                                     //   the callback (NimBLEScan.cpp:155-157), so no
                                     //   unbounded vector growth in a forever-scan
g_pScan->setInterval(62.5 / 0.625);  // window == interval → 100% duty (optional, +25%)
g_pScan->setWindow  (62.5 / 0.625);
g_pScan->start(0, nullptr, false);   // 0 = BLE_HS_FOREVER
```

Then delete the `stop()` at `:93`, `SCAN_INTERVAL_MS`, `SCAN_DURATION_SEC`, `g_scan_in_progress`, and
the entire results-sweep block in `update()` (`:419-467`) — with per-packet callbacks there is no
result vector left to sweep. `update()` shrinks to: lost-beacon timeout, then zone/trend/distance off
the current EMA.

A `duration == 0` scan with a callback set is **restarted automatically after a host resync**
(`NimBLEScan.cpp:494-496`), so it is self-healing. Still worth an `if (!g_pScan->isScanning())
g_pScan->start(0, ...)` guard in `update()` as belt-and-braces.

**Cost: power.** Today the radio is off for a good part of every 500 ms window. This turns it on
continuously. Beacon mode is zoom-gated to 50 m so it is not always-on, which bounds the damage, but
this is a battery device and the trade is real. Worth a battery-drain observation before committing.

#### 7.3b Re-tune in *time*, not in samples ⭐

Do not simply keep `α = 0.4` — it means something different at 5 Hz than at 2 Hz. Derive it from a
time constant using **measured** elapsed ms, since BLE advertising is lossy and the inter-sample gap
will not be a clean 200 ms:

```cpp
const float dt_s = (now - last_sample_ms) / 1000.0f;
const float alpha = 1.0f - expf(-dt_s / TAU_S);
```

This is the direct application of the C7 lesson — *tuning constants measured against a pipeline you
are about to change are provisional*. Today's `α = 0.4` at 500 ms is `τ ≈ 0.98 s`. At 5 Hz you get to
pick a point on the latency/jitter curve; every point below beats today. EMA output jitter scales as
`√(α/(2−α))`, so more samples per time constant is strictly less noise:

| | τ | α @ 200 ms | 90 % settle | jitter vs today |
|---|---|---|---|---|
| today (2 Hz) | 0.98 s | 0.400 @ 500 ms | 2.25 s | 1.00× |
| A — keep α=0.4 | 0.39 s | 0.400 | **0.90 s** | 1.00× (but updates 2.5× more often, looks twitchier) |
| B — keep today's smoothing | 0.98 s | 0.185 | 2.25 s | **1.57× less** |
| C — **recommended** | 0.50 s | 0.330 | **1.15 s** | **1.12× less** |

Option C is ~2× faster *and* quieter — both dimensions improve, which is why it's the pick. At a
100 ms tag the same τ = 0.5 s gives **1.58× less jitter** at that same 1.15 s, and option B's point
becomes **2.21× less jitter**. That is where reconfiguring the tag pays off: RSSI's ±10 dBm multipath
swings are the real enemy here and more packets is directly more averaging.

`ZONE_CHANGE_SAMPLES` must become **time-based** too (`ZONE_CONFIRM_MS`), or 2 samples silently drops
from 1.0 s of confirmation to 0.4 s and the ring starts flickering between zones. Keep it at 1000 ms
for the first hardware test — change one thing at a time — then try 600 ms once the lower noise floor
is confirmed.

**Note what then dominates**: with option C the EMA contributes 1.15 s and the zone confirmation
1.0 s. The confirmation term becomes the larger half of the latency, which is the argument for 7.3c.

`BEACON_LOST_TIMEOUT_MS = 15000` (`:38`) is also sized for a 2 Hz feed. At 5 Hz you'd know the tag
was gone within ~2 s; 4–5 s is plenty.

#### 7.3c Continuous ring width — the part you actually see ⭐

**`rssi_display` is dead code.** The slow α = 0.25 "analog meter feel" second-stage EMA is computed on
every sample (`:113`) and **nothing reads it**. The ring is purely zone-driven: `ring_width` is a
`switch` on `state.zone` giving 8 / 16 / 28 px (`src/ui/navigation.cpp:488-494`), plus the solid CLOSE
fill. Four states. Even with a perfect 10 Hz feed the user would see three discrete jumps.

Driving ring width continuously from `rssi_display` is the exact analogue of C2 — decouple the
continuous visual channel from the slow discrete one:

- **Ring width** ← `rssi_display`, continuous, fast. No hysteresis, no confirmation delay; it can
  move on every packet because a width that drifts by a pixel reads as *analog*, not as flicker.
- **Sonar tempo** ← `state.zone`, discrete, hysteresis-gated, deliberately slow. Tempo changes
  *must* be confirmed — a beep interval that jitters between 500 and 750 ms sounds broken in a way a
  drifting ring never looks.

> **⚠️ This recommendation was tried and reversed the same day — see §7.5 below.** It was built exactly
> as written (tempo stayed on `state.zone`), then field-tested against the waypoint sonar's brand-new
> continuous tempo and reported as *"not as progressive as the beacon... very difficult to gauge where
> to go"*. The rewrite to a continuous tempo is what actually shipped. The prediction above wasn't
> wrong about discrete tempo being risky — it was wrong about *which* risk was bigger: four audible
> steps turned out to matter more than the jitter a naive continuous tempo could introduce, and the
> jitter itself turned out to be avoidable by driving tempo off `rssi_display` instead of `rssi_ema`.
> Kept here, unedited, as the design record this document exists to preserve — see §7.5 for what the
> field actually required and why.

This bypasses the 7.3b zone-confirm term entirely for the visual, which is why it is likely the
largest *felt* improvement of the three despite being the smallest change.

Either wire it up or delete it — it should not stay as a computed-but-unread field.

#### 7.3d Suspected: active scan is deferring the callback by a full second — ✅ **CONFIRMED from source, 2026-07-31**

**No runtime logging was needed — the library source settles it.** `NimBLEScan.cpp`
(`esp-nimble-cpp` 1.4.x, in `.pio/libdeps/`) fires `onResult` immediately only under

```cpp
if (pScan->m_scan_params.passive || !isLegacyAdv ||
    (advertisedDevice->getAdvType() != BLE_HCI_ADV_TYPE_ADV_IND &&
     advertisedDevice->getAdvType() != BLE_HCI_ADV_TYPE_ADV_SCAN_IND))
```

so for a legacy `ADV_IND` advertiser under **active** scan the callback is withheld until the scan
response arrives, and failing that until `BLE_GAP_EVENT_DISC_COMPLETE` — the full scan duration.
Setting `passive` short-circuits the test on its first term, which is what 7.3a does. The remaining
tag-dependent unknown (is it actually `ADV_IND`?) no longer matters, because passive is now
unconditional. Item closed without instrumenting anything.

The reading also confirmed the other two caps precisely, in the same function:
`if (m_scan_params.filter_duplicates && advertisedDevice->m_callbackSent) return 0;` is the
one-report-per-scan filter, and `if (m_maxResults == 0 && m_callbackSent) erase(...)` is why
`setMaxResults(0)` is safe with duplicates off — each device is freed after its callback, so the
results vector cannot grow without bound during a forever-scan.

**Original text, for the record:**

`setActiveScan(true)` (`:283`) sets `passive = 0`. `NimBLEScan.cpp:143` then fires `onResult`

`setActiveScan(true)` (`:283`) sets `passive = 0`. `NimBLEScan.cpp:143` then fires `onResult`
immediately only if `passive || !isLegacyAdv || advType ∉ {ADV_IND, ADV_SCAN_IND}`. For a legacy
`ADV_IND` advertiser under active scan, the callback waits for the **scan response** — and if the tag
doesn't answer promptly, it is deferred all the way to `BLE_GAP_EVENT_DISC_COMPLETE`, i.e. the full
`SCAN_DURATION_SEC = 1` s.

If true, that is a second full second of latency on top of the 3.3 s, and it would also mean the
"stop early on first hit" optimization at `:93` rarely fires as intended.

**This is a hypothesis, not a measurement** — it depends on the tag's advertising type, which nobody
has looked at. Per the residual trap, do not act on it as fact. The one-line test, before changing
anything: log `advertisedDevice->getAdvType()` and `millis() - scan_start_ms` in `onResult`. If the
type is `ADV_IND` and the delta is ~1000 ms, it's confirmed. 7.3a's switch to passive scanning fixes
it either way and also halves radio traffic, so the test is for the record, not for the decision.

### 7.4 Expected result — ✅ all of §7.3 implemented AND verified on hardware, 2026-07-31

| | before | predicted after 7.3a+b+c | **measured** |
|---|---|---|---|
| sample rate | 2.0 Hz | 5 Hz (10 Hz if the tag is set to 100 ms) | **✅ 4.24–4.37 Hz**, live via `beacon status` (tag still at 200 ms — 100 ms not yet tried) |
| ring response | ~3.3 s, 4 discrete states | continuous, ~1.2 s | not separately timed; qualitatively continuous per `beacon status`'s `rssi_display` field |
| zone/tempo response | ~3.3 s | ~2.2 s, and quieter | tempo mechanism changed — see §7.5, not this table |

**What was built** (`beacon_proximity.cpp`, `beacon_proximity.h`, `navigation.cpp`, `diagnostics.cpp`):

- **7.3a** — one `start(0, ...)` scan running forever, `setDuplicateFilter(false)`,
  `setActiveScan(false)`, `setMaxResults(0)`, window == interval == 100 ms. Deleted: the `stop()` on
  first hit, `SCAN_INTERVAL_MS`, `SCAN_DURATION_SEC`, `g_scan_in_progress`, `g_last_scan_ms` and the
  whole results-sweep block. `update()` no longer polls for scan completion; it recomputes
  zone/trend/distance on the Network Task's own cadence, which the time-based constants below make
  legitimate.
- **7.3b** — both EMAs are τ-based off **measured** elapsed ms. `EMA_TAU_S = 0.5 s` (fast).
  `DISPLAY_TAU_S` shipped at 1.0 s here, then was raised to **2.0 s** the same day — see §7.5, it was
  too fast once tempo started reading from it. `ZONE_CHANGE_SAMPLES` became `ZONE_CONFIRM_MS = 1000`,
  so 1 s of confirmation stays 1 s instead of silently becoming 0.4 s. `BEACON_LOST_TIMEOUT_MS`
  15 s → 5 s.
- **7.3c** — ring width is continuous in `rssi_display` (−90 dBm → 6 px, −65 dBm → 34 px). The two
  decisions *around* it stay discrete and stay hysteresis-gated: draw a ring at all, and switch to the
  solid CLOSE fill. **Sonar tempo did not stay on `state.zone` as designed — see the warning box above
  and §7.5.**
- **Trend was re-derived too.** At the time this was written *nothing but diagnostics read it* — that
  changed hours later, see §7.5, it now drives the sonar beep length. The regression itself: it used
  to run against *sample index* with thresholds in dBm/**cycle**, which is precisely the defect this
  audit is named after — at 5 Hz the old ±2 dBm/cycle would have silently become ±10 dBm/s. Now
  regressed against real time over a 4 s window with thresholds of ±1 dBm/s, chosen from the physics:
  walking at 1.4 m/s with n = 2 gives 8.686·v/d ≈ 0.6 dBm/s at 20 m, 1.2 at 10 m, 2.4 at 5 m.

**Two footguns found and handled while doing it:**

1. `setAdvertisedDeviceCallbacks(cb, wantDuplicates)` calls `setDuplicateFilter(!wantDuplicates)`
   *internally*. Any call to it after the explicit `setDuplicateFilter(false)` silently re-enables
   filtering and puts the feed straight back to 2 Hz. `debugScanAll()` did exactly this on its restore
   path — a `beacon scan` would have quietly halved the sample rate for the rest of the session.
   It now restores every parameter, and stops/resumes the continuous scan around its own one-shot.
2. With duplicate filtering off, `onResult` fires for **every advertisement from every device in
   range**, not just the target. The old body built two heap `String`s per callback
   (`getAddress().toString()` then `toLowerCase()`) before comparing. It now compares
   `NimBLEAddress` directly against a target parsed once at `setEnabled()`.

**New read-out**: `BeaconState::sample_interval_ms` — the measured mean inter-arrival, reported by
`beacon status` and `beacon trend` as both ms and Hz. This is the direct verification of 7.3a.
**✅ Measured live: 4.24–4.37 Hz (mean gap ~230 ms)**, up from ~2.0 Hz (~500 ms) pre-fix, with `Scan
callbacks` climbing at ~89/sec across ~30 nearby devices during the same session.

**Build impact**: RAM 195,600 → 195,968 (**+368 B**, almost all the 48-entry timestamped trend ring),
flash 1,668,007 → 1,668,847 (**+840 B**). (This is the §7.3a–d build only — §7.5 below adds more on
top of it.)

**Still to verify on hardware**: the ring's *feel* (qualitative — was not separately timed), and
**radio power draw** — 100% scan duty is the one genuinely new cost here. It is zoom-gated to 50 m so
it is bounded, but if drain is objectionable, `SCAN_WINDOW_MS` is the single knob to dial back (80 ms
gives 80% duty and should still catch a 200 ms advertiser most of the time).

**Caveats, stated up front — now partially resolved.** The sample-rate gain is capped by the tag's own
advertising interval — 5× only exists if the tag is reconfigured to 100 ms, and **the measured 2.2×
at 200 ms matches what was predicted** (2.5× predicted vs. 2.15× actual — close enough to attribute to
real-world advertisement loss, not a config error). The latency figures for ring/zone response were
derived from the EMA step response and were not separately measured. 7.3a's radio-power cost is also
still unmeasured.

7.3a and 7.3b are one file. 7.3c touches the `DRAW_MAIN` handler, so it needs a re-check of the paint
stage — though the ring is a single `lv_draw_arc` and should not move the 9.3 ms. (Not yet re-checked;
low priority, the ring draw is unlikely to be the frame's bottleneck regardless.)

### 7.5 What the field actually required beyond §7.3 — priority, continuous tempo, trend-driven beep

**Built the same day, hours after §7.3a–d, in response to two field reports that §7.3's design did not
anticipate.** Not originally numbered in this document — recorded here after the fact because both
fixes are substantial and this document is where beacon behaviour is tracked.

**Report 1 — the beacon appeared completely silent.** Root cause was a pre-existing bug, not part of
§7 at all: `updateWaypointFixSonar()` (`navigation.cpp`) called `beacon_proximity::suppressSonar(true)`
unconditionally the instant a waypoint was fixed at 50 m zoom, *before* checking whether the waypoint
was actually in sonar range. A waypoint fixed beyond 50 m therefore produced tempo 0 → `stopSonar()`
while permanently muting the beacon — the beacon looked dead with no error anywhere.

**Fix — beacon takes absolute priority.** A beacon is a thing you are trying to *find*; a fixed
waypoint is an area you are walking into, and its sonar is a secondary convenience. New
`beacon_proximity::isInRange()` (scanning, not found, confirmed zone ≠ OUT_OF_RANGE — so it can't
flicker, the confirmed zone already needs 1000 ms of hysteresis-gated agreement) now makes
`updateWaypointFixSonar()` **release the fixed waypoint outright** the moment it goes true, rather
than merely yielding the buzzer. Yielding wasn't enough on its own: a lingering fix also keeps every
other waypoint hidden from the radar, and would re-claim the buzzer the instant the beacon dipped back
out of range.

**Report 2 — "the rate at which the beeping changes is very difficult to gauge where to go."** This is
where §7.3c's design (tempo stays on `state.zone`, see the warning box in that section) was tried and
found wanting next to the waypoint sonar's brand-new continuous tempo (§8.1e, built the same day). The
beacon's four-step tempo (1500/750/500/250 ms) went through the same rewrite the waypoint sonar just
had: **continuous and linear in dBm**, 1500 ms at −90 dBm → 150 ms at −50 dBm
(`interval = 1500 · 0.1^((rssi+90)/40)`). The zone keeps deciding *whether* to beep; it no longer
decides how fast.

**A companion fix went in at the same time: beep *length* now encodes RSSI trend.** `MovementTrend`
had existed since the v2 redesign and was read by nothing — §7.4 above documented that as still true
at the time. It no longer is: beep duration (already a free parameter of `setSonarInterval()`, costing
nothing to add) is now interpolated continuously from the raw regression slope
(`BeaconState::trend_slope_dbm_s`, exposed for exactly this), saturating at ±2 dBm/s: 30 ms neutral,
±30 ms, floored at 12 ms.

**Both of those continuous mappings had the exact bug §8.1e's design note warns about, and both were
caught and fixed the same day (field report: "the beeping is choppy"):**

- **Tempo was first wired to `rssi_ema`** (τ=0.5s, the *fast* EMA). RSSI wobbles ±3–5 dB standing
  still, and over the 40 dB tempo span that's a ~25% swing in beat period — a continuous tempo only
  glides if the value driving it is itself smooth. **Moved to `rssi_display`, and `DISPLAY_TAU_S`
  raised 1.0 → 2.0 s** (correcting the value from §7.3b above) specifically because the ring's
  1.0 s was too fast once the *sonar* started reading it too. This is the clean statement of the
  principle: `rssi_ema` is for things with their own hysteresis downstream (zone, trend), where
  latency costs more than noise; `rssi_display` is for things heard/shown raw, where noise is the
  entire problem and rhythm error is judged more harshly than visual lag.
- **Beep length was first switched on the 3-state `MovementTrend` enum**, not interpolated. Standing
  still, the slope hovers near zero and the classifier flips between the three states at random, so
  the beep jumped 60→30→12 ms beat to beat — heard as the rhythm breaking up, not as a signal. Fixed
  by interpolating continuously from the slope itself (final form described above).

**Build impact (§7.5, on top of §7.3a–d's numbers)**: priority fix flash +204 B; choppy-sonar fix flash
+116 B; RAM ±0 for both.

**Verification status**: priority-release and the choppy-fix's *feel* have not been independently
re-tested by ear/observation since shipping — only the underlying §7.3a sample rate has hard measured
numbers (above). The device was confirmed generally healthy (radar, beacon discovery, sound, button)
after a *later*, unrelated regression (§8.1b's `I2C_PROCESS_MS` attempt, see that section) was
reverted — that confirms nothing was broken, not that these specific behaviours were re-verified.

---

## 8. Full-subsystem audit — everything that is not the render

**Added 2026-07-31**, in response to "look at *all* the functionality, not just the render." §7 covers
beacon proximity; this section covers everything else. Each subsystem was read for the same class of
defect the render had — **a rate or a quantization that nobody re-derived after the pipeline around it
changed** — and graded.

| Subsystem | Verdict |
|---|---|
| **Sonar / buzzer** | ✅ Rhythm ✅ verified; tempo now continuous + silences on arrival (⏳ unverified). §8.1 |
| **Beacon proximity** | ✅ §7.3a-d built and **rate verified live (4.24–4.37 Hz)** — continuous passive scan, τ-based EMAs, continuous ring. Plus §7.5 (priority, continuous tempo, trend-driven beep), fixed same day. See §7 |
| Input latency (touch/button) | Adequate for taps, poor for drag. §8.2 |
| GPS | Healthy; one syscall-per-byte inefficiency. §8.3 |
| Compass | ✅ Healthy — no action. §8.4 |
| Battery | ✅ Healthy — no action. §8.4 |

### 8.1 Sonar and buzzer — the rhythm is measurably wobbly ⭐

This is the item to do after §7, and it is arguably more felt, because **rhythm error is far more
perceptible than visual lag.** The ear resolves timing deviations down to ~10 ms; the eye does not
care about 20 ms of frame jitter.

**The load-bearing constraint**: the buzzer is on **TCA9554 EXIO pin 7**, i.e. behind I2C — not a
GPIO. There is no LEDC / hardware PWM path to it. Every edge is a bus transaction and all timing is
software timing. That is *why* the items below matter; on a plain GPIO most of them would be moot.

#### 8.1a The sonar grid re-bases off the actual fire time, so tempo runs flat ⭐

`src/hardware/buzzer.cpp:215`:

```cpp
g_state.sonar_next_beep_ms = now + g_state.sonar_interval_ms;   // ← now, not the ideal grid
```

`now` is when `update()` *happened* to run, which is up to one task period late. So each period is
`interval + (0..20 ms)`:

- **per-beat jitter up to 20 ms** — at the CLOSE-zone 250 ms interval that is **8 %**, well above the
  ~10 ms audibility threshold;
- **the tempo runs systematically flat** by the mean lateness, ~10 ms → 250 ms becomes ~260 ms, ~4 %.

Fix is one character's worth of intent — phase-lock to the grid instead of re-basing:

```cpp
g_state.sonar_next_beep_ms += g_state.sonar_interval_ms;
if ((int32_t)(now - g_state.sonar_next_beep_ms) > 0) {      // fell far behind (e.g. long I2C stall)
    g_state.sonar_next_beep_ms = now + g_state.sonar_interval_ms;   // resync rather than machine-gun
}
```

Per-beat jitter stays bounded by the task period, but the grid stops walking and the average tempo
becomes exact. **XS effort, directly audible.**

#### 8.1b The quantization floor is 20 ms, and the comment says 10

`buzzer::update()` is called from the **I2C Task** (`task_manager.cpp:265`) at
`I2C_PROCESS_MS = 20`. The comment at `buzzer.cpp:28` — *"driven by `buzzer::update()` at 10ms"* — is
stale, and `task_manager.cpp:264` claims *"20ms loop gives precise sonar rhythm"*, which 8.1a shows it
does not.

After 8.1a, 20 ms is the remaining jitter floor. Options, in increasing cost:

1. **Leave it.** With the grid fixed, ±20 ms of edge placement on a stable grid is much less
   objectionable than a walking tempo. Do 8.1a first and re-listen before spending anything here.
2. ~~**`I2C_PROCESS_MS` 20 → 10.** Halves jitter; doubles I2C Task wakeups on Core 0. Cheap, slightly
   wasteful.~~ ❌ **TRIED 2026-07-31 — BROKE THE DEVICE. Do not retry.**
3. **Dedicated FreeRTOS timer / high-priority task** owning only the buzzer edges, at 5 ms. Correct,
   but it needs `i2c_mutex` discipline since the bus is shared with touch and RTC.

### ❌ Option 2 is void — `I2C_PROCESS_MS` cannot be lowered

Field result: **button unresponsive, buzzer silent.** Reverted immediately; revert **confirmed on
hardware** — radar, beacon discovery and sound all back to normal at `I2C_PROCESS_MS = 20`. So the
constant is the confirmed cause, not a coincidence.

The cost analysis above ("cheap, slightly wasteful") was wrong because it only counted **this task's
own CPU**, which is indeed trivial — a non-blocking queue drain and a few timestamp compares. It never
counted the **I2C bus**, which is the actual contended resource, and the failure is on the bus side.

> ⚠️ **The mechanism originally written here was stale, and is corrected as of 2026-07-31.** This
> section used to explain the failure as *"the CST820 touch driver calls `Wire` directly, bypassing
> `i2c_mutex`"*, citing `docs/compass_i2c_constraint.md`. **That is not true of this codebase.** There
> is no `Wire` usage anywhere in `src/` or `include/` — `cst820_read()` goes through
> `i2c_manager::read()` (`src/hardware/display/cst820.cpp:19`) like every other device, serialized by
> the recursive `g_bus_mutex`. The claim describes the pre-ESP-IDF Arduino build, whose evidence is an
> `[E][Wire.cpp:499]` log line this firmware cannot emit.
>
> **What survives**: the empirical result (10 ms broke the button and buzzer; 20 ms restored them,
> both confirmed on hardware) and the conclusion that the bus, not the CPU, is what the change
> overspent. **What does not**: any account of *how*. Plausible remaining candidates — none measured —
> are total transaction volume against a 400 kHz bus shared with an ~11.7 Hz touch poll, mutex
> queueing pushing touch or EXIO writes past their 200 ms acquire timeout, or the doubled wakeup rate
> starving something else on Core 0. **This is a residual wearing a name.** Per this document's own
> rule, do not act on the stale mechanism, do not treat "the `Wire` bypass is gone, so it's safe now"
> as a reason to retry 10 ms, and bracket the actual failure with instrumentation before proposing
> anything against it.

**So `I2C_PROCESS_MS = 20` is a tuned value, not an arbitrary one**, and 20 ms is a hard floor on sonar
timing resolution for as long as the buzzer is driven over the shared bus. Beat steadiness has to be
bought somewhere else:

- **Smooth the interval rather than the clock** — a deadband (ignore changes under ~4%) plus a slew
  limit on the tempo. Costs nothing, needs no bus access, and targets the perceived problem
  ("not metronomic") more directly than edge placement does anyway.
- **Median-filter the RSSI** before the EMA. An EMA is bad at outliers by construction; a single
  multipath null drags it for a whole time constant.
- Option 3 above, which still has to respect the same bus constraint and is therefore not obviously
  safer — it moves *when* edges are driven, not *how often the bus is touched*.

**Generalisable lesson**: on this board, "the CPU cost is negligible" is not a sufficient argument for
raising any rate that touches I2C. The shared bus is the scarce resource — touch, RTC, EXIO and the
buzzer all contend for it — and this rate was raised without anyone measuring what that contention
costs.

**Second lesson, from the correction above**: the *explanation* of a hardware failure decays exactly
like a performance estimate does. This one was inherited from a doc written for a different I2C stack
and repeated into three files before anyone checked whether the code still looked like that. A
verified symptom and an unverified cause are different things and should never be written in the same
voice.

#### 8.1c Every buzzer edge costs two I2C transactions instead of one

`on()` and `off()` both do `i2c_manager::exio::readOutput()` **then** `exio::set()`
(`buzzer.cpp:78-84`, `:94-100`) — a read-modify-write of a register whose contents we already know,
because we are the only writer at runtime.

At the CLOSE-zone 250 ms interval that is 8 edges/sec × 2 = **16 transactions/sec** of avoidable
traffic on the 400 kHz bus shared with the CST820 touch controller. Caching the output byte in
`exio_state` and writing only would halve it.

**Caveat that makes this a real decision, not a freebie**: EXIO also holds `LCD_RST`, `TP_RST` and
`LCD_CS`. Those are set during init and not touched afterwards, so a cache is safe *today* — but it
makes the cached byte the single source of truth, and any future code that writes EXIO without going
through it silently corrupts the buzzer state. Only worth doing if 8.1b option 3 is chosen and the
edge rate goes up.

#### 8.1d Waypoint sonar has no hysteresis — the tempo flickers when you stand still ⭐

`src/ui/navigation.cpp:804-808`:

```cpp
if      (distance_m <=  5.0f)  interval_ms = 250;
else if (distance_m <= 10.0f)  interval_ms = 500;
else if (distance_m <= 30.0f)  interval_ms = 750;
else if (distance_m <= 50.0f)  interval_ms = 1500;
```

Hard boundaries, evaluated at 10 Hz against a GPS position that jitters by **±2–5 m even with a good
fix**. Stand still at ~10 m from the waypoint and the interval flips between 500 and 750 ms at random.
That is not a subtle artifact — a beep tempo alternating between two rates sounds broken.

The beacon path already solved exactly this: ±3 dBm hysteresis plus N-consecutive-reading confirmation
(`beacon_proximity.cpp:181-243`). **The waypoint path has neither.** It should have both — a few
metres of hysteresis per boundary, and a confirmation hold — ported from the beacon's `classifyRSSI`
shape.

This is the single clearest bug in the audit and it is independent of everything else.

#### 8.1e Waypoint zones are 4 steps of wildly uneven width — ✅ done 2026-07-31, ⏳ unverified

The zones above are 5 m, 5 m, 20 m, 20 m wide. The 10–50 m band — where you spend most of an approach
— is just two tempi, so for most of the walk the sonar tells you nothing is changing. Then it
quadruples in the last 10 m.

Same fix as §7.3c: **map distance to interval continuously** (e.g. geometric between 1500 ms at 50 m
and 250 ms at 2 m) rather than in steps. With 8.1d's hysteresis no longer needed for a continuous
mapping — a continuously drifting tempo doesn't flicker, it glides — this may *replace* 8.1d rather
than follow it. Decide which after hearing 8.1d.

Note the pairing with §7.3c: **the visual channel and the audio channel want the same treatment for
the same reason.** Continuous for the analogue quantity, discrete-with-hysteresis only where a
discrete decision is genuinely being made.

**Outcome — it replaced 8.1d, and field testing said why.** 8.1d was verified working (the tempo no
longer flickered) but the report on the same session was that the waypoint sonar "feels very chaotic,
a lot of beeping even at a further distance, is not as progressive as the beacon". Both halves of that
are 8.1e, not 8.1d: the ladder was steady and still wrong.

Implemented in `navigation.cpp` (`waypointSonarInterval`, `updateWaypointFixSonar`):

- **Continuous geometric mapping**, 2000 ms at 50 m → 250 ms at 2 m. The far end was deliberately
  slowed from the 1500 ms the section originally proposed, because "too much beeping far out" was half
  the complaint — the old ladder gave 750 ms at 30 m, the curve gives ~1420 ms.
  `t = ln(d/2) / ln(25)`, `interval = 250 · 8^t`. Equal distance *ratios* map to equal tempo *ratios*,
  so halving the distance always doubles the tempo wherever you are, and a constant walking pace gives
  a steadily accelerating beat instead of two plateaus and a lurch.
- **The zone enum, the ±3 m hysteresis and the 1 s confirmation hold are all deleted.** A continuous
  tempo has no rates to flicker between.
- **What replaces them is a τ = 1.5 s EMA on the distance**, using measured `dt` (`α = 1 − e^(−dt/τ)`),
  not a sample count — the render-request rate is ~10 Hz with a fix but is not guaranteed, and a
  sample-based EMA would silently change its time constant with it. This is the correct place for the
  guard: smooth the noisy input, not the derived output. ~2 m of lag at walking pace, comfortably
  inside the ±2–5 m GPS error it is filtering.
- **One discrete decision survives, so it keeps its hysteresis**: engage ≤ 50 m, release > 55 m.
- Calling `setSonarInterval()` every cycle with a drifting value is safe — it only re-bases the beat
  grid when the sonar was not already active (`buzzer.cpp:155`), so the glide changes the period
  without resetting §8.1a's phase.

**Also fixed here: the sonar had no arrival stop.** `Waypoint::found` already existed and was already
set by tapping the fixed waypoint within 15 m (`navigation.cpp`, `handleTapAt`) — the exact
counterpart of tapping the beacon ball. `updateWaypointFixSonar()` simply never read it, so arriving
gave no way to silence the beeping short of unfixing the waypoint or leaving 50 m zoom, while the
beacon has silenced-on-found all along. It reads the flag now. This was not in the audit; it surfaced
from field use.

**Build impact**: flash +1,132 B, RAM ±0.

#### 8.1f Dead code: blocking `rapidPulse()`

`buzzer.cpp:137-148` blocks for ~60 ms in `delay()`. It was superseded by `rapidPulseAsync()`
(`:150`) and has no remaining callers — `button.cpp:152` uses the async form. Delete it before someone
calls it from the UI Task.

### 8.2 Input latency — fine for taps, poor for drags

Both input paths are polled **once per UI Task loop**, and with a GPS fix that loop is one frame
(~85 ms):

- **Button** — `device_manager::updateButton()` at `task_manager.cpp:150`, deliberately placed before
  the mutex so it isn't stuck behind the queue drain. ~85 ms worst case. Already verified fine
  outdoors: a real press is 132–186 ms, so nothing is missed. **No action.**
- **Touch** — read by LVGL's indev timer *inside* `lv_timer_handler()` (`:162`), so it inherits the
  same rate. LVGL's `LV_INDEV_DEF_READ_PERIOD` is irrelevant; the handler cannot sample faster than
  it is called. **Effective touch sampling ~11.7 Hz.**

11.7 Hz is fine for taps — a tap is a single edge. It is poor for **drag and scroll**: a swipe through
the settings lists gets 2–3 sample points, so the gesture reads as coarse and steppy rather than as
tracking the finger. It is better than it sounds on the settings screen, because the radar's paint and
background fill are not in that frame — expect ~17–20 Hz there — but it is still the one place a user
touches the device continuously rather than discretely.

If it is worth fixing, the fix is the C2 pattern again: **poll the touch controller on its own
cadence, decoupled from the render**, and feed LVGL from the latest cached sample. That is a real
change (CST820 reads must respect the shared-bus constraint in `docs/compass_i2c_constraint.md`) and
should only be taken on if scrolling actually annoys someone. **Measure the annoyance before paying
for it** — this is exactly the kind of item the render work showed can be over-estimated from code
reading alone.

### 8.3 GPS — healthy, one inefficiency

Sampling is correct: 10 Hz gate (`GPS_UPDATE_INTERVAL_MS = 100`) against the BH-880's native 10 Hz
NAV-PVT rate, and `read()` drains the whole UART buffer each call, so nothing backs up.

The one flaw is how it drains (`gps_bh880.cpp:328`):

```cpp
while (s_uart_installed && uart_read_bytes(GPS_UART, &c, 1, 0) > 0) {
```

**One `uart_read_bytes()` syscall per byte** — each taking the driver's ring-buffer lock. At 115200
baud with 10 Hz NAV-PVT that is on the order of 1–3 k calls/sec. A bulk read into a local buffer
followed by a byte loop over it would cut the syscall count ~100×.

This is on **Core 0**, so it never touches the render, and the device works. Low priority, but it is
free CPU sitting on the floor next to the compass and I2C tasks.

**✅ Implemented 2026-07-31** — 256-byte chunked read, refilling while more is queued. The UBX state
machine is unchanged and still byte-wise; parser state already lived in statics across `read()`
calls, so a chunk boundary falling mid-message costs nothing. +88 B flash, +256 B System Task stack
(8 KB available), ±0 static RAM. **Not yet verified on hardware** — in the §0 field batch.

⚠️ **The syscall saving is reasoned, not measured.** Nobody has instrumented `read()` before or
after, so "~100×" is a call-count ratio, not a time saving — the actual CPU recovered is unknown and
could be small. Do not quote a millisecond figure for this. (See "The residual trap".)

~~Also stale: the comment at `task_manager.cpp:~1205`...~~ ✅ fixed in the §4.1 hygiene pass.

### 8.4 Healthy — no action

**Compass.** The QMC5883L runs continuous at **200 Hz ODR with 512× oversampling**
(`compass_qmc5883l.cpp:49-51`), read at 10 Hz. Reading 1-in-20 samples looks wasteful but isn't: with
OSR 512 the chip's own output is already heavily averaged, so each read is clean rather than a random
instantaneous sample. The heading EMA was correctly re-derived when the rate changed — 1.5° → 0.5°
(`task_manager.cpp:717`) — which is precisely the discipline §7.3b is asking for on the beacon side.
**This subsystem is the example the others should follow.**

**Battery.** 15-sample ADC averaging (`battery.cpp:26`, `:259-267`), 30 s history interval, 5 s
display updates. Nothing about battery is user-perceptible in real time and the filtering is sound.

### 8.5 One cross-cutting consequence worth knowing

`full_refresh = 1` is load-bearing for the zero-copy flush (C7 constraint 1), and it means **any pixel
change costs a full-screen rotate (~38 ms)** — a single label update on the settings screen is as
expensive as a whole radar frame. LVGL still only flushes when something is invalidated, so an idle
screen costs nothing. This is inherent to the zero-copy path, not a defect, but it is why UI outside
the radar does not feel proportionally faster than the radar does.

### 8.6 Recommended order — ✅ all six steps executed 2026-07-31; here's how it actually went

1. ~~**§8.1d** — waypoint sonar hysteresis.~~ ✅ verified, then **superseded by §8.1e**.
2. ~~**§8.1a** — phase-lock the sonar grid.~~ ✅ verified on hardware.
3. ~~**§8.1e** — continuous waypoint tempo.~~ ✅ built, ⏳ unverified (needs an outdoor GPS test). It
   did subsume 1. Field testing after 1 and 2 confirmed the ladder was *steady and still wrong* — see
   the outcome note in §8.1e.
4. ~~**§7.3a–c** — the beacon rate work.~~ ✅ built **and rate-verified live (4.24–4.37 Hz)**. Turned
   out to need a follow-on the plan didn't anticipate: §7.5 (beacon priority + continuous tempo +
   trend-driven beep), built the same day after field-testing 3 and 4 back to back surfaced that the
   beacon's tempo needed the same continuous-mapping treatment §8.1e had just given the waypoint
   sonar — directly contradicting §7.3c's original "keep tempo discrete" design. See §7.5 for the
   full account, including a self-inflicted "choppy" regression in the first cut of that follow-on.
5. ~~**§8.1f** — delete `rapidPulse()`.~~ ✅ done.
6. **§8.1b / §8.2 / §8.3** — of the three, only §8.1b was tried, on the grounds that step 3's field
   report ("choppy") pointed at it. ❌ **It broke the device on hardware** (button unresponsive, buzzer
   silent) — reverted same day. Root cause was I2C bus contention with the touch driver, not the CPU
   cost the change was reasoned about; see §8.1b for the corrected analysis. §8.2 and §8.3 remain
   untried, and this result is a reason for more caution before trying them, not less.

**This was mostly measured on hardware, unlike when this section was written.** Steps 1, 2, 4 and the
§8.1b attempt all now have hardware results — three confirmations and one revert. Step 3's *outdoor*
GPS behaviour and steps 5's, 26's and 28's *feel* remain unverified by ear/observation, only their
build correctness is confirmed. The render effort's own record (§6, "the two cheapest changes
delivered the most, and the largest rewrite returned the least") remains the standing warning, and
§8.1b's revert is this document's newest example of it: the *reasoning* behind a change (CPU cost)
can be sound in isolation and still miss the resource that actually breaks.

---

## ⚠️ MEASURED — 2026-07-28: `lv_canvas_fill_bg` found as the real bottleneck

**The stage-by-stage capture tables from this measurement session were trimmed 2026-08-07** (preserved
in git history as of that date) — the numbers they established are superseded by the final pipeline
described in CLAUDE.md's Render Pipeline section and the C1–C7 entries above. What's worth keeping is
the finding itself, because the *shape* of the mistake recurs elsewhere in this document (see "The
residual trap" below):

An initial A/B test with rotation fully disabled still cost 346 ms/frame — worse than the ~149 ms this
document had estimated *with* rotation — which looked like it disproved software rotation as the
bottleneck. It didn't: `lv_canvas_fill_bg()` was costing **205 ms** (59% of the frame) clearing the
radar canvas to one colour, and that cost dominated and masked whatever rotation was contributing.
Root cause was in LVGL itself — `lv_canvas.c`'s true-colour fill path calls `lv_img_buf_set_px_color`/
`_set_px_alpha` per pixel (460,800 out-of-line calls/frame) instead of using a fast memset path, at
~2.25 MB/s, ~20× below the PSRAM bus. Swapping it for `lv_color_fill()` over `dsc->data` dropped it to
21.3 ms (9.6×) and the whole paint stage 215.3 → 32.2 ms (6.7×) — commit `fa63a03`, entry **C1** above.
With paint stage now small, rotation's real cost became visible and measurable at ~153 ms, confirming
the original §2.2 hypothesis after all — the rotation-OFF test that appeared to disprove it had simply
been confounded by the bigger, unrelated `fill_bg` cost sitting on top of it.

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

## ✅ MEASURED — 2026-07-28: frame progression through steps 7, 8, 9b, 9

Two more stage-by-stage snapshots were taken here (after the cover-check fix, and again after
transpose tuning + the framebuffer swap). Trimmed 2026-08-07 — preserved in git history as of that
date — since both are superseded by the final numbers already carried in CLAUDE.md's Render Pipeline
section and the C5/C6/C7 entries above. The frame's progression through this stretch: **149.6 ms
(after C5) → 94.3 ms (after C6+C7)**, with tiled rotate falling 64.1 → 47.4 ms and the flush to
framebuffer falling from 33.9 ms to effectively zero (deleted by the framebuffer swap, not reduced).
One fact worth keeping standalone: the boot log confirms flash was **already running QIO**
(`board_build.flash_mode = dio` in `platformio.ini` only governs the ROM bootloader's initial load;
`CONFIG_ESPTOOLPY_FLASHMODE_QIO=y` upgrades it at runtime — see the `flash_mode = dio` comment in
CLAUDE.md's PlatformIO Settings) — the §1.8 DIO→QIO backlog item below was void from the start.

**Frame ~499 ms → 94 ms across the whole effort (~0.8 → ~10 fps).** Rotation and translation both
track the compass/GPS at 5–10 Hz, so the sensor/render relationship inverted over the course of this
effort: render now outruns what feeds it, rather than the other way around.

---

# ⬇ ORIGINAL ANALYSIS (2026-07-27) — HISTORICAL, removed 2026-08-07

**Sections 0–5 (the original 2026-07-27 pre-measurement analysis: "what the actual problem is",
Tiers 0–3, and the "measure first" section) were removed outright on 2026-08-07** — preserved in full
in git history as of that date, not relocated. They were the proposal document this whole effort
worked from before any of it was measured, and two of their confident conclusions were wrong in
instructive ways — most notably, §2.2's rotation call turned out to be right, but only after an
unrelated 205 ms bug (`lv_canvas_fill_bg`, see the MEASURED section above) had first made an A/B test
that disabled rotation look like it disproved the hypothesis. The architectural decisions this
analysis led to (tiled transpose over `sw_rotate`, dropping the canvas, zero-copy framebuffers, the
`clip_corner` fix) are recorded properly in
[ADR-0004](adr/0004-tiled-transpose-display-rotation.md), [ADR-0005](adr/0005-zero-copy-radar-draw-event.md),
[ADR-0007](adr/0007-dual-panel-framebuffers-zero-copy-flush.md), and
[ADR-0008](adr/0008-zero-copy-render-path-invariants.md) — read those for *why* each choice was made.
Costs: use the ✅ MEASURED sections above or CLAUDE.md's Render Pipeline section, never this removed
material.

Section 6 below (the all-steps status table) postdates the original analysis and was kept — it was
actively maintained through the end of the effort, not "preserved as written."

---

## 6. Suggested order of work

| Step | Item | Effort | Expected impact | Risk | Status |
|---|---|---|---|---|---|
| 0 | Instrument (§5) | S | — | — | ✅ done (`perf` / DEV HUD, `8d2ac29`) |
| — | **`fill_bg` → `lv_color_fill` (C1)** | XS | **9.6× on the dominant call** | Low | ✅ done `fa63a03` |
| — | **Decouple heading redraw (§3.2 → C2)** | XS | **1 Hz → 5 Hz rotation** | Low | ✅ done |
| — | Mutex worst case (§3.3) | — | — | — | ✅ resolved by C2 |
| 1 | CPU 240 MHz (§1.1) | XS | **101.5 → 85.2 ms** | Low (power) | ✅ done `feb6f59` |
| — | **Sensor rate 5 → 10 Hz** (compass + GPS) | XS | felt smoothness, not ms | Low | ✅ done `feb6f59` |
| 2 | `bb_invalidate_cache` (§1.4) | XS | ~~High~~ | — | ❌ **void** — flag has no implementation in IDF 5.5 |
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
| 10 | Re-test higher PCLK (§2.4) | S | ~~High (60 Hz)~~ **may be negative** — see §2.4 re-assessment | **Medium-high** | open, deprioritized |
| 11 | Waypoint memory (see ROADMAP) | M | Raises the 50-waypoint cap | Low | ✅ done — cap now 500, see ADR-0022 |
| 12 | Serial flush / recompute-per-frame (§3.5–3.6) | S | Low–medium | Low | ✅ done — §3.5 via step 26, §3.6 via step 11 |
| 13 | Doc reconciliation (§4.1) | S | — | — | open |
| **14** | **Waypoint sonar hysteresis (§8.1d)** | XS | **stops audible tempo flicker** | Very low | ✅ verified, then **superseded by 19** |
| **15** | **Phase-lock the sonar grid (§8.1a)** | XS | **removes 8 % beat jitter + 4 % flat tempo** | Very low | ✅ verified on hardware |
| **16** | **Beacon: continuous passive scan (§7.3a)** | S | **2 → 5 Hz sample rate** | Low (power) | ✅ **verified live: 4.24–4.37 Hz** |
| **17** | **Beacon: τ-based EMA + time-based zone confirm (§7.3b)** | S | **3.3 → 2.2 s, less jitter** | Low | ✅ built; `DISPLAY_TAU_S` corrected 1.0→2.0s by step 28 |
| **18** | **Beacon: continuous ring width from `rssi_display` (§7.3c)** | S | **4 states → continuous** | Low | ✅ built, ring ⏳ unverified; **tempo did NOT stay discrete as designed — see step 28** |
| **19** | **Continuous waypoint sonar tempo + arrival stop (§8.1e)** | S | **subsumed step 14**; progressive approach, calmer far out, silences on arrival | Low | ✅ built, ⏳ unverified |
| 20 | Delete blocking `rapidPulse()` (§8.1f) | XS | — (hygiene) | Very low | ✅ done |
| 21 | Beacon: confirm active-scan callback deferral (§7.3d) | XS | — (diagnostic) | — | ✅ **confirmed from source**, no code needed |
| 22 | ~~Buzzer tick 20 → 10 ms (§8.1b)~~ | XS | — | **broke hardware** | ❌ **VOID — tried, button dead + buzzer silent. I2C bus contention, not CPU.** |
| 23 | Decouple touch polling from the render (§8.2) | M | drag/scroll feel | Medium | open, justify first |
| 24 | GPS bulk UART read (§8.3) | S | Core 0 CPU only | Low | open |
| **25** | **Panel ISR core check (§1.5)** | XS | — (diagnostic) | — | ✅ **CONFIRMED: ISR on core 1, shared with uiTask** |
| **26** | **`Serial` fflush gating (§3.5)** | S | ~~reliability~~ **small efficiency** — premise was wrong, see §3.5 | Low | ✅ built and verified |
| 27 | Bounce-buffer A/B — remove for 18.75 KB SRAM (§1.4) | S | SRAM; ms unknown | Medium | open, measure first — now linked to step 25, same A/B answers both |
| **28** | **Beacon absolute priority + continuous sonar tempo + trend-driven beep (§7.5)** | S | fixes "beeping is choppy" and "silent beacon" field reports | Low | ✅ built same day as 16–18; priority + choppy-fix feel ⏳ not separately re-tested |

Steps 14–19 and 28 are **done**, and none of them were render work. Steps 14, 15 and 20 were XS and
independent of everything else. What's actually still open in this list: 23, 24, 27, and re-listening
verification of 17/18/19/26/28's *feel* rather than their measured numbers.

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
