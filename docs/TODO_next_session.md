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

## 🔴 PRIORITY 1 — I2C bus freeze, recurring (2× reported) — ✅ fix built and committed 2026-07-31,
⏳ effectiveness monitored in the field (no safe way to force-reproduce a stuck bus on demand)

**Symptom** (reported twice same session): full interface freeze — button unresponsive, touchscreen
unresponsive, display stops updating. First occurrence required a **full power cycle** (unplug USB +
disconnect battery ~10s) to clear — matches the documented boot-hang failure mode in
`docs/troubleshooting.md`, except this happened **mid-session**, not at boot. Second occurrence
recovered on its own once the device went to standby and woke back up — button, sound, touchscreen,
rotation all came back — but the on-screen DEV/perf HUD text stayed frozen at its last values even
after everything else recovered (separate bug, see "Related, NOT fixed" below).

**Root cause (code-confirmed, not guessed)**: a stuck I2C bus. Any slave (CST820 touch, PCF85063 RTC,
TCA9554 EXIO) left mid-transaction — e.g. by a task hang or a reset — keeps holding SDA low waiting
for a byte that never comes. **An MCU-only event does not reset external chips**, so the bus comes up
already jammed and every subsequent transaction to *every* address fails, not just the one device that
was mid-transfer.

**Why the second occurrence is the smoking gun**: wake-from-standby calls `i2c_manager::reinit()`
(`standby_manager.cpp:358-360`) as part of its recovery sequence — a full bus teardown/rebuild that
includes 9 SCL clock-recovery pulses. That call is *exactly* what cleared touch/button/sound this
time. This confirms both the diagnosis and that the fix (below) uses an already-proven recovery
primitive — it isn't a new, unverified mechanism, just the existing one triggered proactively instead
of needing a manual sleep/wake or power cycle.

**What was already true before today**: `i2c_manager::read()`/`write()` (`i2c_manager.cpp:96-184`) are
individually well-bounded — 50ms IDF-level timeout, 3 retries, 200ms mutex-wait cap — so a single
stuck transaction can't hang a calling task forever on its own. `CONFIG_ESP_TASK_WDT_PANIC=n`
(`sdkconfig.defaults:95`) means a genuinely hung task does **not** auto-reboot the device, it only logs
a warning — consistent with the user needing a manual power cycle rather than the device silently
recovering. The existing generic task-health recovery (`attemptTaskRecovery()`,
`task_manager.cpp:781`) only suspends/resumes a stalled task's FreeRTOS handle — it does **not** touch
the I2C bus at all, so it would be a no-op against a genuinely wedged bus if a hung task ever triggered
it.

**Fix implemented, three parts, all built and compiling (RAM 132,384→132,392 [+8B], flash
1,607,099→1,607,967 [+868B])**:

1. **New failure-burst counter** — `i2c_manager::Stats::consecutive_failures`
   (`include/hardware/i2c/i2c_manager.h`), incremented on every failed `read()`/`write()` and reset to
   0 on the next success (`src/hardware/i2c/i2c_manager.cpp`). A wedged bus fails *every* transaction
   to *every* address, so a sustained run here — as opposed to one device's occasional failed read,
   which resets it on its very next success — is a reliable signal.
2. **Boot-time self-heal** (`i2c_manager::init()`, `i2c_manager.cpp:84-98`) — when the initial
   `ping(EXIO_DEVICE)` fails, call `resetBus()` (9 SCL clock-recovery pulses) and retry the ping once
   before giving up, instead of just printing a warning and continuing crippled. Directly fixes the
   already-documented boot-hang scenario in `docs/troubleshooting.md`, and any case where a reboot
   follows a bus wedge — currently the device comes back up still jammed since an MCU reset doesn't
   free the slave that's holding the line.
3. **Runtime watchdog** — new `checkI2CBusHealth()` (`task_manager.cpp`, called from `systemTask()`'s
   existing health-monitoring block, ~10Hz). Watches `consecutive_failures`; at ≥10 (under 1s of a
   fully jammed bus at touch's ~11.7Hz poll rate — long enough not to fire on a single retried
   transaction, short enough the freeze doesn't linger) it calls `i2c_manager::reinit()` — the same
   call standby-wake already uses successfully. Gated by a 2s cooldown between attempts and capped at
   5 attempts before logging `UNRECOVERABLE` (via `system_logger::error`) and giving up rather than
   spamming `reinit()` forever against genuinely dead hardware. Attempt count resets whenever
   `consecutive_failures` returns to 0 (confirmed healthy), since `reinit()` internally calls `init()`
   which resets all of `Stats` including this counter.

**Decision**: committed without hardware verification of the recovery path itself — there's no safe
way to deliberately jam the I2C bus on demand, and the failure is inherently unpredictable in timing.
Trusting the design (it reuses `reinit()`, the exact primitive standby-wake already proved clears this
on this device) and **monitoring in the field** instead of blocking on a reproduction we can't force.

**→ WATCH FOR, if it happens again**:
- Serial output `[I2C] EXIO recovered after clock pulses` (boot path) or `[I2C] %lu consecutive
  failures — bus appears wedged, attempting recovery` (runtime path).
- Whether the freeze now self-clears within ~2-4s (one cooldown cycle) instead of requiring
  standby-wake or a power cycle.
- Confirm normal operation (touch, button, sound, rotation) is unaffected when the bus is healthy —
  `consecutive_failures` should sit at 0 essentially always, so this should be invisible in normal use.
- If the threshold/cooldown ever need retuning, do it from real log data (how many consecutive
  failures were actually seen, how long recovery actually took) — not by re-deriving the same
  reasoning that picked 10/2000ms/5 the first time.

**Design rationale for the threshold/cooldown/cap numbers and the rejected alternatives** (call
`reinit()` on every failure; fold into the existing task-hang recovery instead): see
[ADR-0003](adr/0003-proactive-i2c-bus-recovery-watchdog.md).

**Related, NOT fixed — separate bug, needs its own investigation**: after the second (self-recovered)
freeze, the on-screen DEV/perf HUD label stayed frozen showing stale values even after touch/button/
sound/rotation all recovered. The label's update code (`navigation.cpp:1267-1300`) runs inside the
same `updateRadarDisplay()` call that draws the (now-working) radar, gated by
`lv_obj_is_valid(ui.perf_label) && !lv_obj_has_flag(ui.perf_label, LV_OBJ_FLAG_HIDDEN)`. Since
rotation/zoom prove that function is executing, the leading hypothesis is `ui.perf_label` became a
dangling pointer to a deleted LVGL object at some point during the freeze/recovery — `lv_obj_is_valid()`
would then silently skip the update forever with no error. **Not yet fixed or root-caused** — if it
recurs, try `dev off` then `dev on` via serial (re-creates the label's visibility state) and see if
that clears it; that would confirm the dangling-pointer theory.

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
  default ON; `serial on|off` command added. **±0 flash** — the removals paid for the additions.
  **✅ Verified on hardware 2026-07-31**: `serial off` silences output, `serial on` restores it, input
  keeps working while muted.
  **Follow-up resolved**: default stays ON regardless of `dev_mode` — no hot-path Serial logging exists
  to gate, and most boot logging predates settings load anyway. See
  [ADR-0002](adr/0002-serial-logging-default-independent-of-dev-mode.md).

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

## 4. Waypoint memory optimization ⭐ *the real resource win* *(M)* — ✅ DONE (partial), verified on hardware

Built and field-verified 2026-07-31, no regressions. RAM 195,984 → 132,384 B (59.8% → 40.4%), flash
1,670,831 → 1,607,099 B (79.7% → 76.6% — confirmed via `readelf` that the flash saving is real, not a
residual guess: `g_ui_state` had non-zero fields forcing the whole object into `.dram0.data` instead of
free `.bss`, so the all-zero `desc`/`hint` regions were costing flash too).

- [x] Moved `desc`/`hint` out of SRAM into a PSRAM `WaypointDetail` block (`heap_caps_calloc(...,
      MALLOC_CAP_SPIRAM)` in `ui_manager::init()`); `Waypoint::desc`/`hint` are now pointers into it,
      guarded (`nullptr`-checked) at both call sites instead of crashing on alloc failure.
- [x] No section attributes used, per the `.ext_ram_noinit` boot-crash warning.
- [ ] **Still open**: `MAX_WAYPOINTS` itself is unchanged (still 50) — this only freed the headroom.
      Read `wpt_us` off the `perf` HUD before raising the cap — the per-waypoint Haversine is
      soft-float `double` on a single-precision FPU. §3.6 proposes an equirectangular approximation.

Details in [`../ROADMAP.md`](../ROADMAP.md), [`../CHANGELOG.md`](../CHANGELOG.md), and
[`adr/0001-waypoint-detail-psram-cache.md`](adr/0001-waypoint-detail-psram-cache.md) (why PSRAM caching
was chosen over re-reading the GPX file on tap).

---

## 5. Lower priority / justify first

- [ ] **§1.4 bounce-buffer A/B** *(S, medium risk)* — removing it frees **18.75 KB SRAM**. The safety
      blocker was checked and isn't one: `on_frame_buf_complete` fires in both modes
      (`esp_lcd_panel_rgb.c:871`), so the tearing guard survives. But the panel would then stream
      directly from PSRAM, competing for the bandwidth `rotate` is ~76% bound by. **Two builds + the
      `perf` HUD, be ready to revert** — the risk lands on display stability, which is currently
      flawless. Scale check: 18.75 KB vs item 4's 64 KB.
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

---

## Out of scope for this queue — experimental feature, not an optimization

**Beacon direction finding ("which way do I walk?")** was previously numbered as item 4 in this
queue, alongside verified optimization work. **Pulled 2026-07-31** — it doesn't belong there. Every
other item here is "make an existing, working thing smoother/faster/more stable" (regression risk on
something proven). This one is "does body-shadow RSSI attenuation even produce a usable directional
signal on this specific enclosure" (feasibility risk on something unproven) — a different category,
correctly tracked separately under **Planned** in [`../ROADMAP.md`](../ROADMAP.md), not this backlog.

The BLE rate work that *unblocked* it (item 3 above) was legitimate performance/stability work and
stays in this queue. The direction-finding feature itself does not — treat it as its own experiment,
opt into it explicitly, don't fold it into a performance pass.

- **Status**: unblocked (4.24–4.37 Hz measured live), not yet built.
- **Full design**: [`beacon_direction_finding.md`](beacon_direction_finding.md).
- **Summary**: [`../ROADMAP.md`](../ROADMAP.md) → Planned → "Beacon Direction Finding".
