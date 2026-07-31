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

**Date**: 2026-07-27 (measurements + completions: 2026-07-28; §7 + §8 added 2026-07-31)

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

This bypasses the 7.3b zone-confirm term entirely for the visual, which is why it is likely the
largest *felt* improvement of the three despite being the smallest change.

Either wire it up or delete it — it should not stay as a computed-but-unread field.

#### 7.3d Suspected: active scan is deferring the callback by a full second — **unconfirmed**

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

### 7.4 Expected result

| | today | after 7.3a + 7.3b (option C) + 7.3c |
|---|---|---|
| sample rate | 2.0 Hz | 5 Hz (10 Hz if the tag is set to 100 ms) |
| ring response | ~3.3 s, 4 discrete states | continuous, ~1.2 s |
| zone/tempo response | ~3.3 s | ~2.2 s, and quieter |

**Caveats, stated up front.** The sample-rate gain is capped by the tag's own advertising interval —
5× only exists if the tag is reconfigured to 100 ms, and 2.5× is what today's 200 ms setting yields.
The latency figures are derived from the EMA step response, not measured on hardware; they should be
confirmed the same way the render numbers were. And 7.3a costs radio power on a battery device.

7.3a and 7.3b are one file. 7.3c touches the `DRAW_MAIN` handler, so it needs a re-check of the paint
stage — though the ring is a single `lv_draw_arc` and should not move the 9.3 ms.

---

## 8. Full-subsystem audit — everything that is not the render

**Added 2026-07-31**, in response to "look at *all* the functionality, not just the render." §7 covers
beacon proximity; this section covers everything else. Each subsystem was read for the same class of
defect the render had — **a rate or a quantization that nobody re-derived after the pipeline around it
changed** — and graded.

| Subsystem | Verdict |
|---|---|
| **Sonar / buzzer** | ⭐ **Worst offender after the beacon.** Rhythm drifts, tempo flickers, no hysteresis. §8.1 |
| **Beacon proximity** | ⭐ Rate-starved at 2 Hz. See §7 |
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
2. **`I2C_PROCESS_MS` 20 → 10.** Halves jitter; doubles I2C Task wakeups on Core 0. Cheap, slightly
   wasteful.
3. **Dedicated FreeRTOS timer / high-priority task** owning only the buzzer edges, at 5 ms. Correct,
   but it needs `i2c_mutex` discipline since the bus is shared with touch and RTC.

Do 1, measure by ear, then decide. Do not jump to 3.

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

#### 8.1e Waypoint zones are 4 steps of wildly uneven width

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

Also stale: the comment at `task_manager.cpp:~1205` still says *"Sampling runs at 5Hz
(GPS_UPDATE_INTERVAL_MS)"*. It is 10 Hz. Folds into §4.1 doc reconciliation.

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

### 8.6 Recommended order

1. **§8.1d** — waypoint sonar hysteresis. Clearest bug in the audit, independent of everything else.
2. **§8.1a** — phase-lock the sonar grid. XS, directly audible.
3. **§7.3a–c** — the beacon rate work. Largest single improvement, three related changes in ~two files.
4. **§8.1e** — continuous waypoint tempo. Do after hearing 1 and 2; it may subsume 1.
5. **§8.1f** — delete `rapidPulse()`. Trivial hygiene.
6. **§8.1b / §8.2 / §8.3** — only if 1–4 leave something still feeling wrong. All three are
   speculative-benefit and should be justified by observation, not by reading this document.

**None of this is measured on hardware.** Every claim above is derived from reading the code, and the
render effort's own record (§6, "the two cheapest changes delivered the most, and the largest rewrite
returned the least") is the standing warning about how badly that can go. The audibility arguments in
§8.1 are the strongest because they rest on arithmetic over constants in the source; §8.2 is the
weakest and is flagged as such.

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

### 1.1 The CPU is running at 160 MHz, not 240 MHz ⭐ biggest single free win — APPLIED 2026-07-28, unverified on hardware

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

#### Applied — 2026-07-28

`CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y` is now in `sdkconfig.defaults`. `CONFIG_PM_ENABLE` is off, so
this is a fixed frequency rather than a DFS ceiling.

**Setting it in `sdkconfig.defaults` was not sufficient.** PlatformIO does not regenerate
`sdkconfig.<env>` when `sdkconfig.defaults` changes. The first build succeeded, reported success, and
still produced a 160 MHz binary. The fix is to delete the generated `sdkconfig.cc-radar` and rebuild.
Always diff the regenerated file against the old one — here it also revealed three settings that had
drifted, including `CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL`, which existed *only* in the generated
file and would have silently reverted NimBLE allocations into internal SRAM. It is now pinned in
`sdkconfig.defaults`.

This failure is the same shape as the original bug: a configuration value that nothing ever printed.
Boot now logs the measured frequency via `getCpuFrequencyMhz()` (`esp_clk_tree`), so a stale
sdkconfig announces itself on the first line of the log.

#### Measured on hardware — 2026-07-28

Boot reports `[BOOT] CPU: 240 MHz`. All four tasks healthy, `I2C Requests: total=0 failed=0`, no
display artifacts, no PSRAM instability. `perf` at 100m zoom, no GPS fix:

| Stage | 160 MHz | 240 MHz | ratio | predicted |
|---|---|---|---|---|
| tiled rotate | 47.4 | **38.3** | 1.24× | "barely moves" ❌ |
| radar bg fill | 21.5 | **20.5** | 1.05× | "barely moves" ✅ |
| LVGL non-radar draw | 23.2 | **17.0** | 1.36× | ~1.5× ≈ ✅ |
| radar paint | 9.4 | **9.3** | 1.01× | ~1.5× ❌ |
| flush | 0.02 | 0.045 | — | — |
| **FRAME** | **101.5** | **85.2** | **1.19×** | ~80ms ≈ ✅ |

*(Baseline is the per-stage sum, 47.4+21.5+23.2+9.4 = 101.5ms. Note the pre-existing doc
inconsistency: `CLAUDE.md` presents that breakdown as "where the 94ms sits", but it sums to 101.5 —
the 94 and the breakdown came from different captures. 101.5 is the like-for-like comparison; against
a best-case 94ms frame the gain is 1.10×.)*

**Two predictions were wrong, and they were wrong in opposite directions:**

1. **Rotate was not at the memory ceiling.** It was predicted to barely move and instead delivered the
   single largest absolute gain, 9.1ms. So ~24% of the transpose was CPU work — loop overhead and the
   scatter into the SRAM tile — not PSRAM bandwidth.
2. **Radar paint is not CPU-bound.** Predicted 1.5×, delivered 1.01× — it did not move at all.
   Emitting geometry into LVGL's draw context is bound by writing the draw buffer, not by computing
   the geometry. Optimizing the drawing *math* would therefore buy nothing.

The correction that matters for future work: **"full-screen PSRAM write" is not one category.** Bg
fill (1.05×) really is at the bus ceiling; rotate (1.24×) was not. The claim in `CLAUDE.md` that both
"sit near the memory ceiling" was true of only one of them — and it was, once again, an
un-instrumented grouping standing in for a measurement (see "The residual trap" below).

**Measurement still owed** — the numbers above were taken **indoors with no GPS fix**, which is the
light case: only the compass drives renders and the deadband suppresses stationary noise (71 flushes
since boot, nowhere near 10 Hz). Still untested:

1. ~~**Frame time and input latency with a GPS fix.**~~ **Verified outdoors 2026-07-29 with satellites
   locked: rotation and button both good. No regression.** The mechanism predicted was real — with a
   fix, `RADAR_REFRESH` is queued every GPS sample, so the UI Task renders nearly every loop and polls
   input once per ~90ms rather than once per 26.6ms vsync — but the *consequence* did not follow.
   ~90ms is comfortably shorter than a real button press (132–186ms in the boot log), so presses are
   never missed and the latency stays below the perceptual threshold for a discrete action.

   **The error worth keeping:** the prediction conflated *poll interval* with *perceived latency*.
   Those coincide for continuous input like a drag, and diverge for a discrete press, where the only
   thing that matters is whether the poll interval is shorter than the event. Don't reason about input
   "feel" from the poll rate alone — ask how long the event being sampled lasts.

   **Worth revisiting later, not now:** dropping the GPS-driven `RADAR_REFRESH` to 5 Hz while leaving
   the compass at 10 Hz would halve the render rate for little visible cost, since translation matters
   less than rotation. Nothing calls for it today — reconsider it if PCLK goes up or a heavy waypoint
   load makes Core 1 the constraint again.
2. **Heavier waypoint load.** All captures above were taken with 2–3 waypoints near the test location
   (`waypoints` = 2.4ms of paint). That is a realistic light case, not an empty one — but the
   filtering docs discuss 50-waypoint scenarios, and waypoint drawing is the one stage that scales
   with content. Worth a `perf` reading on a dense GPX before assuming 85ms is the ceiling. Note the
   distance filter (10× zoom radius) and 8-sector clustering bound the *drawn* count regardless of how
   many are loaded, so the growth should be sublinear.
3. **Battery *runtime*, not gauge calibration.** To be explicit, because the earlier wording was
   ambiguous: **the voltage→percentage calibration does not need redoing.** The thresholds in
   `battery.cpp` (VBAT_MAX 4.12, range 3.0–4.1V, calibrated 2026-02-13) map voltage to state of
   charge, and that mapping is a property of the cell, not of the CPU clock.

   The only clock-related effect on the *reading* is extra sag under load: ~20–30mA more current
   through the cell's internal resistance (~150mΩ) is roughly 4mV, which against the ~1.1V full-scale
   span is under 0.5% — below the gauge's own noise. Not worth acting on.

   What is genuinely unmeasured is **how many hours the device runs on a charge**, since higher clock
   means higher draw. Cheap version, no serial needed (serial requires USB, and USB charges the
   battery — see `memory/hardware_constraints.md`): note the on-screen battery percentage, use the
   device normally for a couple of hours, note it again. Compare against your sense of the previous
   builds.

   Expect the effect to be modest and possibly hard to detect: the backlight and display dominate
   draw on this board, and the radar redraws continuously at either clock. The CPU delta is plausibly
   5–10% of total current, not a halving of runtime.
4. ~~**PSRAM stability.**~~ Checked 2026-07-28: no display artifacts, `I2C Requests: total=0
   failed=0`, no `Wire.cpp requestFrom Error -1`, all four tasks healthy over a 50s run. Worth
   re-checking over a long session, but 240 MHz CPU / 80 MHz octal PSRAM looks clean here.

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

### 1.4 The bounce buffer is destroying the data cache every frame — ❌ **VOID (2026-07-31)**

> **This item is wrong twice over. Do not implement it.** It had been ranked #1 of the remaining
> render work right up until someone read the IDF source.
>
> **1. The flag does not exist as a behaviour.** `bb_invalidate_cache` is declared in
> `esp_lcd_panel_rgb.h:172` and referenced **nowhere** in the implementation — zero hits across all of
> `framework-espidf/components/` on IDF 5.5.0. It is a dead struct field. Setting it compiles, links,
> and does nothing.
>
> **2. The premise contradicts what the driver actually does.** The claim below is that the bounce
> memcpy pollutes the dcache with framebuffer lines that are read once and never reused. But the
> driver *deliberately prefetches* those exact lines:
>
> ```c
> // esp_lcd_panel_rgb.c:841-845
> // Preload the next bit of buffer to the cache memory, this can improve the performance
> if (panel->num_fbs > 0 && panel->flags.fb_behind_cache) {
>     Cache_Start_DCache_Preload(&panel->fbs[panel->bb_fb_index][...], panel->bb_size, 0);
> ```
>
> The framebuffer data is in the cache *on purpose*, so the bounce memcpy hits warm lines. An
> invalidate-after-read would have been fighting the driver's own optimization, not helping it.
>
> **What the dig did turn up** — the bounce buffer costs **18.75 KB of internal SRAM**
> (`system_config.h:35`: 10 lines × 480 px × 2 B × 2 buffers). Disabling it entirely
> (`bounce_buffer_size_px = 0`) frees that, and SRAM at 59.7 % is a genuinely scarce resource here
> where frame time is not. **The safety blocker was checked and is not one**:
> `on_frame_buf_complete` fires in *both* modes — `esp_lcd_panel_rgb.c:871` handles the non-bounce
> path at DMA EOF — so the C7 constraint-4 tearing guard survives.
>
> The trade is real and unresolved, so treat it as a **measurement, not a change**: removing the
> bounce buffer deletes a 460 KB/frame memcpy, but makes the panel stream directly from PSRAM,
> competing for exactly the bandwidth `rotate` is ~76 % bound by — which is the contention the bounce
> buffer was added to prevent. Two builds and the `perf` HUD settle it.
>
> **Scale check before spending time on it**: 18.75 KB versus the ~64 KB the ROADMAP waypoint-memory
> item frees, and the risk lands on display stability, which is currently flawless.
>
> **Third item in this document to evaporate on contact with the source**, after §1.8 (already QIO)
> and §2.4 (higher PCLK may be net-negative). The pattern is consistent enough to be a rule: *before
> implementing any Tier 0 config item, grep the IDF for the flag's implementation.*

**Original text, preserved as written:**

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

#### ⚠️ Re-assessed 2026-07-29: higher PCLK may make the *frame* slower, not faster

This item was written when the frame was 149ms and reads as pure upside. The 240 MHz per-stage
measurements make the trade-off concrete, and it cuts the other way:

The panel continuously fetches the whole framebuffer from PSRAM — 480×480×2 = 460 KB per refresh.

| PCLK | Panel refresh | PSRAM read by panel |
|---|---|---|
| 10 MHz (now) | 37.7 Hz | **17.4 MB/s** |
| 16 MHz | 60.3 Hz | **27.8 MB/s** |

For comparison, the radar bg fill *writes* 460 KB in 20.5ms = **22.5 MB/s**, and that stage was
measured to be bandwidth-bound (it scaled only 1.05× with a 1.5× CPU clock). So the panel at 16 MHz
would be consuming a share of PSRAM bandwidth comparable to everything the render achieves — against
the two stages already proven to be sitting on that exact limit.

**The likely outcome is that rotate and bg fill both get slower**, partially or wholly cancelling the
higher panel rate. What a higher PCLK actually buys is a shorter tearing window and lower
present-latency — not more fps, since fps is set by our 85ms frame, not by the panel's 26.6ms period.

So step 10 is no longer "High impact". Revised view:

- Measure PSRAM contention *before* chasing 60 Hz: raise PCLK and re-read the per-stage `perf`
  numbers. If rotate goes up more than the panel period comes down, revert.
- §1.4 (`bb_invalidate_cache`) becomes more important, not less — it is the one change that reduces
  the panel's effective cache/bandwidth cost rather than adding to it.
- This is also the point where the `on_frame_buf_complete` guard stops being theoretical: margin is
  85ms/26.6ms ≈ 3.2× now, and 16 MHz cuts the panel period to 16.6ms (≈5.1× — the guard gets *more*
  margin from the frame side, but the swap-to-latch window shortens in wall-clock terms).

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
| ~~`CLAUDE.md`: "MCU: ESP32-S3 @ 240MHz"~~ | ~~Builds at **160 MHz**~~ — resolved 2026-07-28, now genuinely builds at 240 (§1.1) |
| `CLAUDE.md`: "framework = arduino", `[env:cc-moat-port]` | `platformio.ini` is `framework = espidf`, `[env:cc-radar]` |
| `CLAUDE.md`: "ESP-IDF version doesn't support bounce buffer" | Bounce buffer is configured and active (§2.4) |
| `CLAUDE.md`: "40-line bounce buffer", "BUFFER_LINES 40/50/120/160" | `BUFFER_LINES = 480` |
| `CLAUDE.md`: "Use full refresh for stability / `full_refresh = 1`" | Code sets `full_refresh = 0` |
| `CLAUDE.md`: partitions "3MB app + 10MB FFat" | Build uses `partitions_ota.csv` — 2×2 MB OTA + 11.7 MB FFat |
| ~~`CLAUDE.md` / memory: compass "~1 Hz"~~ | ~~Read gate is 20 ms, effective ~5 Hz~~ — resolved 2026-07-28: `SYSTEM_UPDATE_MS = 100`, so compass and GPS are both 10 Hz |
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
| 11 | Waypoint memory (see ROADMAP) | M | Raises the 50-waypoint cap | Low | open |
| 12 | Serial flush / recompute-per-frame (§3.5–3.6) | S | Low–medium | Low | open |
| 13 | Doc reconciliation (§4.1) | S | — | — | open |
| **14** | **Waypoint sonar hysteresis (§8.1d)** | XS | **stops audible tempo flicker** | Very low | ✅ built, ⏳ unverified |
| **15** | **Phase-lock the sonar grid (§8.1a)** | XS | **removes 8 % beat jitter + 4 % flat tempo** | Very low | ✅ built, ⏳ unverified |
| **16** | **Beacon: continuous passive scan (§7.3a)** | S | **2 → 5 Hz sample rate** | Low (power) | open ⭐ |
| **17** | **Beacon: τ-based EMA + time-based zone confirm (§7.3b)** | S | **3.3 → 2.2 s, less jitter** | Low | open ⭐ |
| **18** | **Beacon: continuous ring width from `rssi_display` (§7.3c)** | S | **4 states → continuous** | Low | open ⭐ |
| 19 | Continuous waypoint sonar tempo (§8.1e) | S | may subsume step 14 | Low | open |
| 20 | Delete blocking `rapidPulse()` (§8.1f) | XS | — (hygiene) | Very low | ✅ done |
| 21 | Beacon: confirm active-scan callback deferral (§7.3d) | XS | — (diagnostic) | — | open |
| 22 | Buzzer tick 20 → 10 ms (§8.1b) | XS | halves residual jitter | Low | open, do step 15 first |
| 23 | Decouple touch polling from the render (§8.2) | M | drag/scroll feel | Medium | open, justify first |
| 24 | GPS bulk UART read (§8.3) | S | Core 0 CPU only | Low | open |
| 25 | Panel ISR core check (§1.5) | XS | — (diagnostic) | — | open ⭐ |
| 26 | `Serial` fflush gating (§3.5) | S | **reliability**, not ms | Low | open ⭐ |
| 27 | Bounce-buffer A/B — remove for 18.75 KB SRAM (§1.4) | S | SRAM; ms unknown | Medium | open, measure first |

Steps 14–18 are the highest-value open items in this document, and **none of them are render work**.
Steps 14, 15 and 20 are XS and independent of everything else — see §8.6 for the recommended order.

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
