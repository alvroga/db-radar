# cc-radar — work queue (handoff)

**Written 2026-07-31.** Ordered by what I'd actually do next. Full reasoning lives in
[`performance_optimization_backlog.md`](performance_optimization_backlog.md) §7 and §8 — this file is
the short version.

**Last commit**: `a60cb40` — §1.5 confirmed on hardware. Builds clean. RAM 195,984 (59.8%),
flash 1,670,831 (79.7%). On-device state as of this writing: **flashed and verified healthy**
(radar, beacon discovery, sound, button all confirmed working after the `I2C_PROCESS_MS` revert).

**Standing rules for this project**
- Don't commit until the change is verified on hardware (docs-only commits excepted).
- Measure build impact (RAM/flash) on every code change — stash-build-restore for the baseline.
- Never attribute an un-instrumented residual to a hypothesis. Bracket it with `esp_timer_get_time()`.
- Before implementing anything that rests on a claim about ESP-IDF behaviour — a config flag, a
  blocking/non-blocking path, a buffering mode — **read the IDF source first**:
  `grep -rn "<thing>" ~/.platformio/packages/framework-espidf/components/`. **Four** backlog items
  have now been void or misattributed this way, most recently §3.5, whose entire "reliability"
  rationale (a CDC write that stalls) turned out to describe a path that cannot stall.

---

## 0. ✅ DONE — verified on hardware 2026-07-31

Commit `4452718`'s two audible sonar fixes were tested and **both pass**. The beat is steady; the
waypoint tempo holds its rate. That commit is good.

Field testing also produced two findings the audit had missed, both handled in item 1 below: the
waypoint tempo was *steady and still wrong* (not progressive, too busy far out), and there was **no
way to stop the beeping on arrival**.

---

## 1. ✅ DONE (⏳ unverified) — §8.1e continuous waypoint sonar tempo + arrival stop

Built 2026-07-31. It **did subsume** the §0 hysteresis fix — the zone enum, ±3 m hysteresis and 1 s
confirmation hold are deleted. Flash +1,132 B, RAM ±0.

- Continuous geometric tempo, **2000 ms at 50 m → 250 ms at 2 m** (far end slowed past the originally
  proposed 1500 ms, because "too much beeping far out" was half the complaint).
- GPS-noise guard moved to the **input**: τ = 1.5 s EMA on the distance, measured `dt`. Hysteresis
  survives only on the one genuinely discrete decision — engage ≤ 50 m, release > 55 m.
- **Arrival stop**: `updateWaypointFixSonar()` now honours `Waypoint::found`. The flag and its 15 m
  tap-to-set already existed; the sonar just never read it.

**→ TO TEST (outdoor, needs GPS):** walk a fixed waypoint in from ~50 m at 50 m zoom. The tempo should
*glide* continuously faster with no steps, be noticeably calmer than before beyond ~25 m, and go
silent the moment you tap the waypoint within 15 m. Check it re-engages if you unfix/refix.

---

## 2. ✅ DONE — §1.5 confirmed, §3.5 built (⏳ `serial on|off` not separately re-tested)

Built 2026-07-31. Combined flash +264 B, RAM ±0.

- **§1.5 panel ISR core** — ✅ **CONFIRMED on hardware.** `perf` reported
  `Panel ISR: core 1 (uiTask on core 1) — SHARED, competing with render`. The hypothesis was right —
  and the reading covers the bounce-buffer copy too, not just vsync, since the RGB driver services
  both from one interrupt on one core. **Do NOT attribute any of the 83.2ms measured frame time to
  this** — only the core is measured; the magnitude needs a `BOUNCE_BUFFER_LINES = 0` A/B (see backlog
  §1.5 for the reasoning and a new complication: moving the ISR to Core 0 now means sharing it with
  the §7.3a continuous BLE scan's ~89 callbacks/sec).
- **§3.5 `Serial` fflush gating** — done, but **the item's rationale was wrong** and the backlog now
  says so. There is no unbounded CDC stall to protect against: `cdcacm_write` rolls back and drops
  bytes rather than waiting, and the only `portMAX_DELAY` in that driver is on the read side with
  `s_blocking` false by default. The real (smaller) problem was that the unconditional `fflush`
  *defeated* stdout's line buffering. Nine flushes removed; `SerialClass::setLogEnabled()` gate added,
  default ON; `serial on|off` command added. **±0 flash** — the removals paid for the additions. Boot
  logs and all serial commands used throughout the rest of this session worked normally, which
  exercises the gate's default-ON path — but `serial off` → `serial on` specifically has not been
  tried.
  **→ still to test:** confirm `serial off` silences output and `serial on` restores it (input is
  deliberately never gated, so the command works while muted).

**Explicitly do NOT do**: `-O2` (spends flash, the scarcest resource, at 79.5% of a 2 MB OTA slot, to
buy ms that can't be spent) · cache sizes (costs 48 KB SRAM) · higher PCLK (may be net-negative) ·
grid-as-rects (paint measured 1.01× on a 1.5× clock — not compute-bound) · `bb_invalidate_cache`
(**void**, no implementation in IDF 5.5).

---

## 3. ✅ DONE AND VERIFIED — §7 beacon BLE rate work

Built 2026-07-31, all of §7.3a–d. RAM +368 B, flash +840 B. **Item 4 is now unblocked.**

- **§7.3a** — one continuous passive scan, duplicates off, `setMaxResults(0)`, 100 ms window ==
  interval. Poll loop, `stop()`-on-hit, `SCAN_INTERVAL_MS`, `SCAN_DURATION_SEC`, `g_scan_in_progress`
  and the results sweep all deleted. **✅ Verified live: `beacon status` measured 4.24–4.37 Hz**
  (mean gap ~230ms, was ~500ms), `Scan callbacks` climbing at ~89/sec across ~30 nearby devices with
  the target-MAC count climbing alongside it.
- **§7.3b** — both EMAs τ-based off measured `dt`; zone confirmation is a duration (1000 ms);
  `BEACON_LOST_TIMEOUT_MS` 15 s → 5 s. **Trend was re-derived too.**
  ⚠️ **`DISPLAY_TAU_S` changed again after this** — see item 3c below, it's now 2.0s not 1.0s.
- **§7.3c** — ring width continuous in `rssi_display` (−90 dBm → 6 px, −65 → 34 px). The two discrete
  decisions around it (draw at all / solid CLOSE fill) keep their hysteresis.
- **§7.3d** — **confirmed from the NimBLE source, no code needed.** Active scan on a legacy `ADV_IND`
  advertiser genuinely does withhold `onResult` until the scan response. Passive fixes it.

**Still open:**
- [ ] Ring should now *grow smoothly* as you approach, not jump between four thicknesses.
- [ ] **Battery drain.** 100% scan duty is the one genuinely new cost, zoom-gated to 50 m.
      If it's bad, `SCAN_WINDOW_MS` is the single knob (80 ms → 80% duty).
- [x] Run `beacon scan` once, then re-check `beacon status` sample rate — verifies the
      `debugScanAll()` restore path doesn't silently re-enable duplicate filtering. Done as part of
      the diagnostic session (`fb9af73`, `0d27d9b`) that chased down a dead-tag false alarm.

**⚠️ Tag advertising interval**: was briefly set to 100 ms, then reverted to 200 ms after the tag
appeared to go silent — that turned out to be the tag genuinely not advertising (see item 3c), not
the interval change. **Confirm what the tag is currently set to before assuming 200 ms.**

### 3b. Findability follow-up — ✅ done, superseded by 3c below

Field report after 3: beacon appeared silent (a fixed waypoint was muting it), and the beeping was
*"very difficult to gauge where to go"*.

- **Beacon now has absolute priority** — `isInRange()` releases any fixed waypoint outright.
  **→ still to test:** fix a waypoint, walk into beacon range, confirm the fix releases and the
  beacon takes over the buzzer.
- ~~Beacon sonar tempo is continuous, linear in dBm: 1500 ms @ −90 → 150 ms @ −50~~ and
  ~~beep duration encodes trend: 60/30/12 ms~~ — **both were choppy in the field, see 3c.**

### 3c. Choppy-sonar fix — ✅ done, ⏳ not separately re-tested by ear

The mechanics in 3b were both self-inflicted noise sources — the exact "discrete/noisy value driving
something meant to sound continuous" defect being fixed everywhere else that day.

- Tempo now driven by **`rssi_display`** (not `rssi_ema`) — `rssi_ema`'s ±3-5dB standing-still wobble
  was a ~25% swing in beat period over the 40dB tempo span.
- `DISPLAY_TAU_S` raised **1.0 → 2.0s** because of the above.
- Beep length now **continuously interpolated** from `trend_slope_dbm_s`, saturating at ±2 dBm/s
  (30ms ± 30ms, floored at 12ms) — the old 3-state enum switch flipped at random near zero slope,
  making the beep length jump 60→30→12ms beat to beat.

**→ TO TEST:** hunt the beacon and listen specifically for whether the tempo now feels like a smooth
glide (not a jitter) and the beep-length change feels like a trend signal (not noise).

### 3d. `I2C_PROCESS_MS` 10ms attempt — ❌ VOID, reverted, confirmed fixed

Tried halving the buzzer's 20ms timing floor (§8.1b) to further help 3c. **Broke the device**: button
unresponsive, buzzer silent. Root cause: I2C bus contention with the CST820 touch driver, which
bypasses `i2c_mutex`, not I2C Task CPU cost (which is what the change was reasoned about). Reverted to
20ms; **user-confirmed on hardware**: "regular functioning radar, beacon discovery and sound" restored.
`I2C_PROCESS_MS = 20` is now a documented hard floor — do not retry raising this rate. See backlog
§8.1b and `memory/i2c_process_ms_floor.md`.

**Separately surfaced while chasing this**: a stuck-I2C-bus boot hang (freezes right after
`[I2C] Initialized: SDA=15...`) that only a **full power cycle** clears, not a reset. Confirmed on
hardware, documented in `docs/troubleshooting.md`. Watch for this after rapid reset/reflash cycles.

**If beacon smoothness still needs work**: the tick-rate lever is now closed. Remaining options are a
**deadband + slew limit on the tempo** and a **median filter on RSSI before the EMA** — neither
touches the I2C bus. See the "room for improvement" discussion recorded in conversation on 2026-07-31.

**If it's still hard to gauge** after 3c, the next lever is the tag at 100 ms (doubles the sample
rate, halves trend latency) — then `TREND_WINDOW_MS` can drop from 4000 to ~2500 without losing slope
confidence, which is what makes "warmer/colder" respond within a step or two rather than after
several.

---

## 4. Beacon direction finding — "which way do I walk?" *(M)* — **UNBLOCKED**

~~Blocked on item 3~~ — item 3 is built and **verified: 4.24–4.37 Hz measured live** (confirms the
"~5 Hz" case below, not yet the 100 ms/10 Hz case). At 2 Hz a rotation yielded 1.7 samples per 30° bin
(noise); at the confirmed ~4.3 Hz it yields ~4, and at 10 Hz (tag reconfigured to 100 ms) 8.3, which
gives 7–14 dB of SNR. Full design in
[`beacon_direction_finding.md`](beacon_direction_finding.md).

True BT 5.1 AoA is **impossible** here (single antenna, no CTE IQ). Body-shadow DF works: the body
attenuates 2.4 GHz by 10–20 dB, and the QMC5883L supplies heading per sample.

- [ ] Publish `latest_heading` global from System Task; read it in the NimBLE `onResult` callback.
      100 ms staleness = 3.6° error at a 10 s rotation. No sync machinery needed.
- [ ] Accumulate `X += rssi*cos(θ)`, `Y += rssi*sin(θ)`, `W += |rssi|`. Bearing = `atan2(Y,X)`,
      confidence = `sqrt(X²+Y²)/W`. **Not `argmax`.**
- [ ] Per-bin counts for a coverage gate — refuse until every sector has ≥ K samples.
- [ ] **⚠️ Calibrate the sign empirically.** Body shadowing says peak = beacon direction, but the
      device's own asymmetric pattern may offset or invert it. Place the beacon at a known bearing,
      rotate, record where the peak lands. Repeat at 10/25/40 m. **Do not derive this.**
- [ ] Confidence gate must **refuse** rather than guess. Expect ±30–45° outdoors at 10–40 m;
      unreliable indoors.
- [ ] UI mode + bearing arrow.

**Optional complement**: GPS gradient DF — log `(lat, lon, rssi)` while walking, trilaterate over two
~15 m legs. More robust outdoors, no user ritual, and could be built first.

---

## 5. Waypoint memory optimization ⭐ *the real resource win* *(M)*

**If the goal is banking resources, this is the highest-yield item in the project** — ~8× more SRAM
than anything in the render backlog, and it lifts a real user-facing limit.

`g_ui_state` is the largest symbol in the firmware at **70,992 B** (~37% of static RAM), almost all of
it `desc[1024]` + `hint[256]` × 50 waypoints — read in exactly one place, for one waypoint at a time.

- [ ] Move `desc`/`hint` out of SRAM (PSRAM via `ps_malloc()` in `ui_manager::init()`, or re-read from
      the GPX file on tap). **Frees ~64 KB**, or buys ~500 waypoints in the same budget.
- [ ] **⚠️ Do NOT use section attributes** — `.ext_ram_noinit` boot-crashes on this IDF because
      constructors aren't called there. Keep hot fields (~300 B) in SRAM, move only `Waypoint[]`.
- [ ] Read `wpt_us` off the `perf` HUD *before* raising the cap — the per-waypoint Haversine is
      soft-float `double` on a single-precision FPU. §3.6 proposes an equirectangular approximation.

Details in [`../ROADMAP.md`](../ROADMAP.md).

---

## 6. Lower priority / justify first

- [ ] **§1.4 bounce-buffer A/B** *(S, medium risk)* — removing it frees **18.75 KB SRAM**. The safety
      blocker was checked and isn't one: `on_frame_buf_complete` fires in both modes
      (`esp_lcd_panel_rgb.c:871`), so the tearing guard survives. But the panel would then stream
      directly from PSRAM, competing for the bandwidth `rotate` is ~76% bound by. **Two builds + the
      `perf` HUD, be ready to revert** — the risk lands on display stability, which is currently
      flawless. Scale check: 18.75 KB vs item 5's 64 KB.
      **Now linked to §1.5** (confirmed 2026-07-31: the panel ISR — which is doing this exact
      PSRAM→SRAM bounce copy at ~17.4 MB/s — shares Core 1 with `uiTask`). The same
      `BOUNCE_BUFFER_LINES = 0` build answers both items' unmeasured magnitude in one A/B.
- [x] ~~**§8.1b buzzer tick 20 → 10 ms**~~ — ❌ **VOID. Tried 2026-07-31, broke the device** (button
      unresponsive, buzzer silent), reverted. The change was justified on CPU cost, which is trivial;
      the binding constraint is **I2C bus contention** with the CST820 touch driver, which bypasses
      `i2c_mutex`. `I2C_PROCESS_MS = 20` is tuned, not arbitrary. See §8.1b.
      → Beat steadiness must come from a **tempo deadband + slew limit** and a **median filter on
      RSSI** instead, neither of which touches the bus.
- [ ] **§8.3 GPS bulk UART read** — `gps_bh880.cpp:328` does one `uart_read_bytes()` syscall **per
      byte**. Core 0 only, never touches the render. Free CPU on the floor.
- [ ] **§8.2 decouple touch polling** *(M)* — touch samples at ~11.7 Hz (inside `lv_timer_handler`).
      Fine for taps, coarse for settings scrolling. **Weakest claim in the audit — justify by actual
      annoyance before paying for it.**
- [ ] **§8.1c buzzer EXIO read-modify-write** — every edge costs 2 I2C transactions. Only worth it if
      the tick rate goes up; makes the cached byte the single source of truth for EXIO.
- [ ] **§4.1 doc reconciliation** — several stale comments, incl. "Sampling runs at 5Hz" in
      `task_manager.cpp` (it's 10 Hz) and the buzzer's "driven at 10ms" (it's 20 ms).

---

## Things that are NOT problems — verified healthy 2026-07-31

Don't re-audit these.

- **Compass** — 200 Hz ODR, 512× OSR, read at 10 Hz. Its EMA *was* correctly re-derived when the rate
  changed (1.5° → 0.5°). This is the example the other subsystems should follow.
- **Battery** — 15-sample ADC averaging, 30 s history. Sound.
- **Button input** — ~85 ms worst case, verified outdoors; a real press is 132–186 ms.
- **Render** — 85.2 ms/frame outruns the 10 Hz sensor feed. Four flags are load-bearing and must not
  be "cleaned up": `clip_corner` OFF, `radar_obj` not `CLICKABLE`, `full_refresh` tied to rotation
  mode, and the `on_frame_buf_complete` guard. See CLAUDE.md.
