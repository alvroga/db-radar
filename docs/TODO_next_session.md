# cc-radar — work queue (handoff)

**Written 2026-07-31.** Ordered by what I'd actually do next. Full reasoning lives in
[`performance_optimization_backlog.md`](performance_optimization_backlog.md) §7 and §8 — this file is
the short version.

**Last commit**: `4452718` — sonar fixes + full-subsystem audit. Builds clean. RAM 195,600 (59.7%),
flash 1,666,611 (79.5%).

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

## 2. ✅ DONE (⏳ unverified) — §1.5 + §3.5

Built 2026-07-31. Combined flash +264 B, RAM ±0.

- **§1.5 panel ISR core** — instrumented. `on_vsync_cb` records `xPortGetCoreID()` into a volatile
  (recorded, not printed — `printf` is not ISR-safe); `perf` reports
  `Panel ISR: core N (uiTask on core M) — SHARED / separate`.
  **→ TO READ:** run `perf` once and report the line. If SHARED, §1.5's suggested fix (create the
  panel from a Core 0 task) is justified; if separate, delete §1.5.
- **§3.5 `Serial` fflush gating** — done, but **the item's rationale was wrong** and the backlog now
  says so. There is no unbounded CDC stall to protect against: `cdcacm_write` rolls back and drops
  bytes rather than waiting, and the only `portMAX_DELAY` in that driver is on the read side with
  `s_blocking` false by default. The real (smaller) problem was that the unconditional `fflush`
  *defeated* stdout's line buffering. Nine flushes removed; `SerialClass::setLogEnabled()` gate added,
  default ON; `serial on|off` command added. **±0 flash** — the removals paid for the additions.
  **→ TO TEST:** confirm boot logs still appear normally, `serial off` silences them, and `serial on`
  restores them (input is deliberately never gated, so the command works while muted).

**Explicitly do NOT do**: `-O2` (spends flash, the scarcest resource, at 79.5% of a 2 MB OTA slot, to
buy ms that can't be spent) · cache sizes (costs 48 KB SRAM) · higher PCLK (may be net-negative) ·
grid-as-rects (paint measured 1.01× on a 1.5× clock — not compute-bound) · `bb_invalidate_cache`
(**void**, no implementation in IDF 5.5).

---

## 3. §7 — beacon BLE rate work ⭐ *the big one* *(S, ~2 files)*

**The largest felt improvement remaining, and the hard prerequisite for item 4.** Not a CPU problem —
240 MHz bought it nothing.

Today the feed is capped at **2.0 Hz** by three things in `beacon_proximity.cpp`: NimBLE's default
controller-side duplicate filtering, `g_pScan->stop()` on first hit (`:93`), and
`SCAN_INTERVAL_MS = 500` (`:18`). The tag advertises at **200 ms** and can be set to **100 ms**.

- [ ] **§7.3a Continuous passive scan** — `setActiveScan(false)`, `setDuplicateFilter(false)`,
      `setMaxResults(0)`, `start(0, ...)`. Delete the `stop()`, `SCAN_INTERVAL_MS`,
      `SCAN_DURATION_SEC`, `g_scan_in_progress`, and the results-sweep block (`:419-467`). Optionally
      set window == interval for 100% duty. **→ 2 Hz becomes 5 Hz (10 Hz if the tag is reconfigured).**
      *Cost: radio power. Zoom-gated to 50 m, so bounded — but observe battery drain.*
- [ ] **§7.3b Re-tune in time, not samples** — `α = 1 - expf(-dt_s / TAU_S)` using **measured**
      elapsed ms, because BLE advertising is lossy. Recommended τ = 0.5 s (~2× faster *and* quieter
      than today). Make `ZONE_CHANGE_SAMPLES` time-based too, or 1.0 s of confirmation silently
      becomes 0.4 s. Drop `BEACON_LOST_TIMEOUT_MS` from 15 s to ~5 s.
- [ ] **§7.3c Continuous ring width** — `rssi_display` is computed every sample (`:113`) and **read by
      nothing**; the ring is 4 discrete states off `state.zone` (`navigation.cpp:488-494`). Drive width
      continuously from `rssi_display`, keep the sonar tempo on the hysteresis-gated zone. *Likely the
      largest felt change of the three.* Re-check the paint stage after (single `lv_draw_arc`, should
      not move the 9.3 ms).
- [ ] **§7.3d** *(XS, diagnostic)* — log `getAdvType()` and `millis() - scan_start_ms` in `onResult`
      to confirm or kill the suspected active-scan callback deferral. 7.3a fixes it either way; this is
      for the record.

**Also set the tag to 100 ms** before item 4.

---

## 4. Beacon direction finding — "which way do I walk?" *(M)*

**Blocked on item 3.** At 2 Hz a rotation yields 1.7 samples per 30° bin (noise); at 10 Hz it yields
8.3, which gives 7–14 dB of SNR. Full design in
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
- [ ] **§8.1b buzzer tick 20 → 10 ms** — do item 0 first and re-listen; with the grid phase-locked,
      the residual jitter may not be objectionable.
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
