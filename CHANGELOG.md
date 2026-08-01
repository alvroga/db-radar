# Changelog - cc-radar Development History

All notable completed features and changes to the GPS Radar project.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

---

## [Unreleased]

### Changed

**Waypoint `desc`/`hint` moved from SRAM to PSRAM — frees ~64KB static RAM** — ✅ *verified on
hardware, no regressions*

`g_ui_state` was the largest symbol in the firmware (70,992 B, ~37% of static RAM), almost all of it
`desc[1024]` + `hint[256]` × 50 waypoints — read in exactly one place (`waypoint_screen.cpp`, one
waypoint at a time) but resident for all 50 permanently. Added a `WaypointDetail` struct holding just
those two fields, allocated once as a single block via `heap_caps_calloc(MAX_WAYPOINTS,
sizeof(WaypointDetail), MALLOC_CAP_SPIRAM)` in `ui_manager::init()`; `Waypoint::desc`/`hint` became
`char*` pointers into it instead of embedded arrays. No section attributes (`.ext_ram_noinit`
boot-crashes on this IDF); allocation failure is handled by leaving the pointers `nullptr` rather than
crashing, guarded at both the one write site (`gpx_loader.cpp`) and the one read site
(`waypoint_screen.cpp`). See [ADR-0001](docs/adr/0001-waypoint-detail-psram-cache.md) for why PSRAM
caching was chosen over re-reading the GPX file on tap.

**Flash dropped too, not just RAM — confirmed via `readelf`, not assumed.** `g_ui_state` has some
non-zero-initialized fields (e.g. `current_zoom`'s default enum value), so the whole object —
including the large all-zero `desc`/`hint` regions — was being placed in `.dram0.data` (a PROGBITS
section: its zero bytes are stored as literal zeros in flash and copied to RAM at boot) rather than
the free `.dram0.bss`. A `git stash` + `readelf -S` diff on the ELF showed `.dram0.bss` byte-identical
before/after; the entire saving came out of `.dram0.data`, which is why shrinking this struct paid off
in both partitions at once — not a coincidence, and not attributed without checking.

`MAX_WAYPOINTS` is still 50 — this frees the headroom but does not itself raise the cap; that remains
a separate follow-up (see ROADMAP).

**Build impact**: RAM 195,984 → 132,384 B (59.8% → 40.4%), flash 1,670,831 → 1,607,099 B
(79.7% → 76.6%).

**Code references**: `include/ui/ui_manager.h` (`WaypointDetail`, `Waypoint::desc`/`hint`),
`src/ui/ui_manager.cpp` (`init()` allocation), `src/gpx/gpx_loader.cpp` (write site),
`src/ui/waypoint_screen.cpp` (read site).

### Fixed

**Beacon sonar was choppy — the continuous tempo and trend-beep from the previous entry both had a
self-inflicted noise problem** — ⏳ *fix built, not separately re-verified by ear*

Field report right after the priority/continuous-tempo work below: "the beeping is choppy." Both
causes were introduced in that same change, and both are the exact defect being fixed everywhere else
that day — a discrete or noisy value driving something meant to be heard as continuous.

1. **Tempo was driven by `rssi_ema` (τ=0.5s).** RSSI wobbles ±3-5 dB standing still, and over the
   40 dB tempo-mapping span a 4 dB swing is a ~25% change in beat period. A continuous tempo only
   reads as a *glide* if the value driving it is itself smooth — otherwise "continuous" just means
   "jittering constantly" instead of "stepping occasionally," which is worse than the four-step
   version it replaced. Tempo now comes from `rssi_display` instead, and `DISPLAY_TAU_S` was raised
   1.0 → 2.0s. This makes the EMA split explicit as **decision vs presentation**: `rssi_ema` is fast
   because zone and trend have their own hysteresis/confirmation downstream, so latency hurts there
   and noise doesn't; `rssi_display` is slow because the ring and the sonar tempo are shown/heard raw,
   where noise is the entire problem — deliberately slower than the ring alone would want, because
   rhythm error is far more perceptible than visual lag.
2. **Beep length switched on the three-state `MovementTrend` enum.** Standing still, the regression
   slope hovers near zero, so the classifier flipped APPROACHING/STABLE/DEPARTING at random and the
   beep length jumped 60→30→12ms beat to beat — heard as the rhythm breaking up, not as information.
   Now interpolated continuously from the raw slope (`BeaconState::trend_slope_dbm_s`), saturating at
   ±2 dBm/s: 30ms neutral ±30ms, floored at 12ms.

**Build impact**: flash +116 B, RAM ±0.

### Fixed

**`I2C_PROCESS_MS` 20 → 10ms broke the button and buzzer — reverted** — ✅ *verified on hardware:
radar, beacon discovery and sound all confirmed back to normal at 20ms*

Attempted to halve the sonar's 20ms timing-quantization floor (backlog §8.1b) by doubling the I2C
Task's rate. On hardware: **button unresponsive, buzzer silent.** Reverted immediately.

The cost analysis behind the change was wrong in a specific and generalisable way — it counted the I2C
Task's own CPU cost (a non-blocking queue drain plus a few timestamp compares, genuinely trivial) and
never counted the actual contended resource: the **I2C bus**. The CST820 touch driver calls `Wire`
directly, bypassing `i2c_mutex` entirely (the same constraint documented in
`docs/compass_i2c_constraint.md`, which is why the compass can't be read from the I2C Task either).
Doubling this task's rate doubles the collision rate against an already-contended bus, so the EXIO
writes driving the buzzer and the button's own reads start failing.

**`I2C_PROCESS_MS = 20` is therefore a tuned floor, not an arbitrary constant**, and 20ms is a hard
limit on sonar timing resolution (~8% jitter at a 250ms interval) for as long as the buzzer is driven
over the shared bus. Marked void in the backlog with the reasoning, so it isn't retried. Beat
steadiness has to come from smoothing the *interval* (deadband + slew limit) or the RSSI *input*
(median filter before the EMA) instead — neither touches the bus.

**Separately discovered while debugging this**: a stuck I2C bus can hang boot completely, immediately
after `[I2C] Initialized: SDA=15...` — a slave (touch or RTC) left holding SDA low across an MCU reset
jams the bus, and only a **full power cycle** (not a reset) releases it, since the MCU reset doesn't
power-cycle the slaves. Confirmed on hardware and documented in `docs/troubleshooting.md`.

**Build impact**: RAM ±0, flash ±0 (constant reverted to its original value).

### Changed

**Beacon takes absolute priority, and its sonar became continuous + trend-aware** — ⏳ *priority logic
awaiting hardware verification; the tempo/trend-beep mechanics below were superseded within hours by
the choppy-sonar fix above (`rssi_display` instead of `rssi_ema`, continuous beep length instead of
the fixed 60/30/12ms), which is where the current behaviour is described*

Field report after the §7 rate work: the beacon appeared *silent*, and once that was explained, the
beeping was *"very difficult to gauge where to go"*. Two separate causes.

**1. A fixed waypoint could mute the beacon entirely.** `updateWaypointFixSonar()` called
`suppressSonar(true)` unconditionally as soon as a waypoint was fixed at 50 m zoom, and *then*
computed the tempo — so with the waypoint beyond 50 m the tempo was 0 → `stopSonar()`, leaving the
beacon permanently muted by something that made no sound itself. The continuous-tempo rewrite already
fixed that path, but the priority was still backwards.

**The beacon now wins outright.** A beacon is a thing you are trying to *find*; a fixed waypoint is an
area you are walking into, and its sonar is a secondary convenience. When `beacon_proximity::isInRange()`
becomes true the waypoint fix is **released**, not merely out-prioritised — leaving it fixed would keep
every other waypoint hidden from the radar and re-take the sonar the moment the beacon dipped out of
range. Safe against flicker: `isInRange()` reads the *confirmed* zone, which needs 1000 ms of
hysteresis-gated agreement to enter.

**2. The beacon sonar still had the four-step defect just removed from the waypoint sonar.** Tempo was
a `switch` on `state.zone` — 1500/750/500/250 ms — so most of a search happens *inside* one zone, where
moving produces no audible change at all. That is fatal for a beacon specifically: someone hunting is
not judging absolute loudness, they are listening for *change in response to their own movement*, and
a step function gives them none until they cross a boundary.

Tempo is now continuous and **linear in dBm** — 1500 ms at −90 dBm to 150 ms at −50 dBm,
`interval = 1500 · 0.1^((rssi+90)/40)`. Linear in dBm is the correct curve rather than an
approximation: RSSI ≈ C − 20·log₁₀(d), so equal dBm steps are equal *ratios* of distance — the same
geometric mapping the waypoint sonar uses over metres, reached from the other direction. The zone
still decides *whether* to beep; it no longer decides how fast.

**Trend is finally used for something.** It had been computed since the v2 redesign and read by
nothing. "Warmer/colder" is far more actionable than absolute level when hunting, because absolute
RSSI depends on the environment, the tag's orientation and your own body — but the *sign of the
change* is meaningful regardless. The buzzer is a bare on/off line with no pitch to modulate, but beep
*duration* is already a parameter, so this cost nothing: **60 ms** tone when APPROACHING, **30 ms**
neutral, **12 ms** clipped tick when DEPARTING.

**Build impact**: RAM ±0, flash 1,668,847 → 1,669,051 (**+204 B**).

**Beacon proximity: 2 Hz → ~5 Hz, and everything derived from that rate re-derived with it** — ✅
*verified on hardware 2026-07-31*: `beacon status` measured **4.24–4.37 Hz** live (mean gap ~230ms,
up from ~500ms), with `Scan callbacks` climbing at ~89/sec across ~30 nearby devices and the target
MAC count climbing alongside it.

Backlog §7 in full (7.3a–d). The BLE feed was capped at exactly **2.0 Hz** against a tag advertising
at 5 Hz, so about 60% of every advertisement was thrown away. Three independent causes, all removed:

1. **Duplicate filtering** — NimBLE reports a given advertiser to `onResult` once *per scan* while
   `filter_duplicates` is set.
2. **`g_pScan->stop()` on the first hit** — ended the scan the instant the beacon was seen,
   guaranteeing exactly one sample per cycle.
3. **A 500 ms scan/idle poll loop** in `update()`.

Now a **single continuous passive scan** (`start(0, …)` → `BLE_HS_FOREVER`), duplicates off,
`setMaxResults(0)`, 100 ms window == interval. The poll loop, `SCAN_INTERVAL_MS`,
`SCAN_DURATION_SEC`, `g_scan_in_progress` and the results-sweep block are all deleted.

**Passive is load-bearing, not just a power choice.** With active scanning and a legacy `ADV_IND`
advertiser, NimBLE *withholds* `onResult` until the scan response arrives, and failing that until the
scan completes. This had been filed as §7.3d "suspected, confirm with runtime logging" — reading
`NimBLEScan.cpp` confirms it outright, so the item closed without instrumenting anything.

**The more important half: every constant derived from the old rate was re-derived.** Left alone,
each would have silently changed meaning by 2.5–5× the moment the feed sped up — which is exactly the
defect (*a rate constant nobody re-derived after the pipeline around it changed*) that the audit was
named after.

- Both EMAs are **τ-based from measured elapsed time** (`α = 1 − e^(−dt/τ)`, τ = 0.5 s fast / 1.0 s
  display) rather than fixed per-sample α. Measured, because BLE advertising is lossy and a dropped
  advertisement must widen that sample's weight rather than skew the time constant.
- Zone confirmation is a **duration** (`ZONE_CONFIRM_MS = 1000`) rather than "2 consecutive samples",
  which meant 1.0 s only because samples arrived at 2 Hz.
- Trend slope is regressed against **real time in dBm/s** over a 4 s window rather than against sample
  index in dBm/cycle. Thresholds ±1 dBm/s, taken from the physics: `8.686·v/d` at 1.4 m/s and n = 2
  gives 0.6 dBm/s at 20 m, 1.2 at 10 m, 2.4 at 5 m. (Diagnostic only — nothing acts on trend.)
- `BEACON_LOST_TIMEOUT_MS` 15 s → 5 s, which at the new rate is already 25–50 missed advertisements.

**The ring is continuous now (§7.3c).** Width interpolates from `rssi_display` (−90 dBm → 6 px,
−65 dBm → 34 px) instead of snapping between four zone-selected widths. `rssi_display` — the slow EMA
that exists specifically to drive it — had been computed every sample and **read by nothing at all**.
The two decisions *around* the ring stay discrete and keep their hysteresis: whether to draw one, and
whether to switch to the solid CLOSE fill.

**Two footguns found and fixed while doing this**:
`setAdvertisedDeviceCallbacks(cb, wantDuplicates)` calls `setDuplicateFilter(!wantDuplicates)`
internally, so `debugScanAll()`'s restore path would have silently put the feed back to 2 Hz for the
rest of the session after any `beacon scan`; and with duplicates off `onResult` fires for every
advertisement from every device in range, where the old body built two heap `String`s per callback
before rejecting — it now compares a `NimBLEAddress` parsed once.

`beacon status` / `beacon trend` report the **measured** mean inter-arrival in ms and Hz — the direct
verification that this worked. **Still to check on hardware: radio power draw**, since 100% scan duty
is the one genuinely new cost. `SCAN_WINDOW_MS` is the single knob if it's objectionable.

**Build impact**: RAM 195,600 → 195,968 (**+368 B**, almost all the 48-entry timestamped trend ring),
flash 1,668,007 → 1,668,847 (**+840 B**).

**`Serial` no longer flushes on every call, and logging can be switched off** — ⏳ *awaiting hardware
verification*

Backlog §3.5 was filed as *reliability*: `fflush(stdout)` was believed to stall unboundedly on the
USB CDC path when no host is draining, blocking whichever task logged. **Checking the IDF 5.5 source
before implementing — the standing rule that has now caught four items — showed the premise is
false.** `cdcacm_write` → `esp_usb_console_write_buf` → `esp_usb_console_flush_internal` →
`cdc_acm_fifo_fill`, which rolls back and returns 0 when the host isn't draining. It silently drops
bytes; it never waits. The only `portMAX_DELAY` wait in that driver is on the read side, and its
`s_blocking` is false by default. **There is no stall on this console, in or out.**

The fix is still right, for a smaller reason. stdout is line buffered (IDF returns `S_IFCHR` from
`_fstat_r_console`), so the unconditional `fflush` was not merely redundant — it *defeated* the line
buffering: `print("x"); print(1); print("\r\n")` became three `cdcacm_write` calls where one would do,
and that function loops the VFS layer one byte at a time taking a recursive lock per byte.

- All nine unconditional `fflush(stdout)` calls removed. Explicit `Serial.flush()` remains.
- Added `SerialClass::setLogEnabled(bool)`, checked **before** formatting in every overload (the
  numeric ones gate before their `snprintf`). Default ON — nothing changes unless asked. Its value is
  field/battery mode, where no host is attached and every log line is wasted CPU.
- New `serial on | serial off` command. Input is never gated, so it still works with logging off, and
  `serial off` prints its confirmation before muting.

**Build impact**: **±0 flash, ±0 RAM** — deleting the nine flushes paid for the gate and command exactly.

### Added

**Panel ISR core probe (`perf`)** — ⏳ *result not yet read off hardware*

Backlog §1.5 hypothesises that the RGB panel's DMA/vsync ISR is installed on Core 1 — the same core
as `uiTask` — because `esp_intr_alloc()` binds to whichever core calls it and the panel is created
from the boot path, which sdkconfig pins to Core 1. This is a measurement, not a change:
`on_vsync_cb` records `xPortGetCoreID()` into a volatile (recorded rather than printed — `printf` is
not ISR-safe), and `perf` reports `Panel ISR: core N (uiTask on core M) — SHARED / separate`. One
reading either justifies moving display init to Core 0 or deletes the hypothesis.

**Build impact**: flash +264 B, RAM ±0.

**Waypoint sonar: continuous tempo, and it finally stops when you arrive** — ⏳ *awaiting hardware
verification*

The hysteresis fix below was verified working — and field testing in the same session showed it had
fixed the wrong layer. The report: the waypoint sonar "feels very chaotic, a lot of beeping even at a
further distance, is not as progressive as the beacon", and there was no way to stop it on arrival.
The ladder was steady and still wrong.

**1. Distance→tempo is now continuous, not four zones** (`navigation.cpp`). The old zones were
5/10/30/50 m — 5 m, 5 m, 20 m, 20 m wide — so the 10–50 m band where most of an approach happens was
only two tempi. You walked 40 m, heard one rate, one step, one rate. Now a geometric mapping,
**2000 ms at 50 m → 250 ms at 2 m**, `interval = 250 · 8^(ln(d/2)/ln 25)`. Equal distance *ratios*
give equal tempo *ratios*, so halving the distance always doubles the tempo and a constant walking
pace yields a steady acceleration rather than two plateaus and a lurch. The far end was deliberately
slowed past the 1500 ms originally proposed — "too much beeping far out" was half the complaint, and
the old ladder gave 750 ms at 30 m where the curve gives ~1420 ms.

**This replaced the zone hysteresis rather than building on it.** A continuous tempo has no rates to
flicker between, so the zone enum, the ±3 m hysteresis and the 1000 ms confirmation hold are all
deleted. The GPS-noise guard that replaces them is a **τ = 1.5 s EMA on the distance** using measured
`dt` (`α = 1 − e^(−dt/τ)`) rather than a sample count — the render-request rate is ~10 Hz with a fix
but is not guaranteed to be, and a sample-based EMA would silently change its time constant with it.
That is the right place for the guard: smooth the noisy input, not the derived output. One discrete
decision survives — beeping vs silent — so it keeps hysteresis: engage ≤ 50 m, release > 55 m.

**2. Arrival stop.** `Waypoint::found` already existed and was already set by tapping the fixed
waypoint within 15 m — the exact counterpart of tapping the beacon ball. `updateWaypointFixSonar()`
simply never read it, so arriving gave no way to silence the beeping short of unfixing the waypoint
or leaving 50 m zoom, while the beacon has silenced-on-found all along. It reads the flag now. Not
from the audit; this one surfaced from field use.

**Build impact**: RAM 195,600 (**±0**), flash 1,666,611 → 1,667,743 (**+1,132 B**).

### Fixed

**Sonar rhythm: the beat grid was walking, and the waypoint tempo had no hysteresis** — ✅ *verified
on hardware 2026-07-31* (the hysteresis half is now superseded by the continuous tempo above)

Found by a full-subsystem audit (2026-07-31) prompted by the question "did anyone check anything
other than the render?" — the answer was no; the whole optimization effort had been scoped to frame
time. Two independent defects, both audible, neither related to CPU speed.

**1. The sonar beat grid re-based off actual fire time** (`buzzer.cpp`). `sonar_next_beep_ms = now +
interval` used `now` — whenever `update()` happened to run, up to one I2C Task period (20 ms) late.
Every period was therefore `interval + (0..20 ms)`:

- per-beat jitter up to 20 ms — **8% at the 250 ms CLOSE interval**, well above the ~10 ms the ear
  resolves;
- tempo systematically **~4% flat**, because the grid absorbed the mean lateness instead of holding.

Now advances by a fixed step (`+= interval`) so the average tempo is exact, with a catch-up guard
that resyncs rather than firing backdated beeps if a stall puts it more than one interval behind.

**2. Waypoint proximity sonar had no hysteresis at all** (`navigation.cpp`). Distance→tempo was a
bare if/else ladder with hard boundaries at 5/10/30/50 m, evaluated at the 10 Hz sensor rate against
a GPS position that jitters ±2–5 m even with a good fix. Standing still near a boundary made the
tempo flip between two rates at random — a beep interval alternating between 500 and 750 ms sounds
broken.

Replaced with a zone state machine mirroring the one `beacon_proximity` already used for RSSI:
**±3 m hysteresis** on the exit threshold of the current zone, plus a **1000 ms confirmation hold**
before a zone change commits. Both guards are needed — hysteresis alone still flips on one large GPS
excursion, confirmation alone still flips on sustained jitter across the boundary. Zone state resets
on every disengage path so re-engaging never inherits a stale zone.

### Removed

**Blocking `buzzer::rapidPulse()`** — spun in `delay()` for ~60 ms and had no callers left after
`rapidPulseAsync()` superseded it. Called from the UI Task it would have stalled LVGL for most of a
frame.

**Build impact**: RAM 195,592 → 195,600 (**+8 B**), flash 1,666,247 → 1,666,611 (**+364 B**).

**Also documented, not yet implemented**: [`docs/performance_optimization_backlog.md`](docs/performance_optimization_backlog.md)
§7 (beacon BLE feed is rate-starved at 2 Hz against a 5 Hz source — not a CPU problem) and §8 (audit
of every remaining subsystem). Plus [`docs/beacon_direction_finding.md`](docs/beacon_direction_finding.md)
— whether the device can tell you which way to walk. Short answer: BT 5.1 AoA is impossible on this
hardware, but body-shadow DF using the compass works, and is blocked on §7 because at 2 Hz a rotation
yields 1.7 samples per 30° bin.

### Performance

**CPU restored to 240MHz, and sensors raised to 10Hz now that the render outruns them**

Two changes that stop the render being faster than the things feeding it. **Frame 101.5 → 85.2ms
(1.19×), verified on hardware** — boot reports `[BOOT] CPU: 240 MHz`, all four tasks healthy, zero
I2C failures, no display artifacts.

| Stage | 160 MHz | 240 MHz | ratio |
|---|---|---|---|
| tiled rotate | 47.4 | 38.3 | 1.24× |
| radar bg fill | 21.5 | 20.5 | 1.05× |
| LVGL non-radar draw | 23.2 | 17.0 | 1.36× |
| radar paint | 9.4 | 9.3 | 1.01× |
| **FRAME** | **101.5** | **85.2** | **1.19×** |

Two predictions in the backlog were wrong in opposite directions, and both are worth carrying
forward: **rotate was not at the memory ceiling** (predicted "barely moves", delivered the largest
absolute gain — ~24% of the transpose was CPU work, not bandwidth), and **radar paint is not
CPU-bound** (predicted 1.5×, delivered 1.01× — it is bound by writing the draw buffer, not by
computing geometry, so optimizing the drawing math would buy nothing). "Full-screen PSRAM write"
turned out not to be one category.

*CPU 160 → 240MHz.* The binary had been running at 160MHz since the ESP-IDF migration: vanilla IDF
defaults to 160 and the Arduino core used to set 240 on our behalf, so the clock was lost silently
when the framework changed. One line in `sdkconfig.defaults`
(`CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y`). `CONFIG_PM_ENABLE` is off, so this is a fixed frequency,
not a DFS ceiling. Expect ~1.5× on CPU-bound work (BLE host, GPS UBX parsing, I2C, compass read,
LVGL draw and radar paint) and much less on the two full-screen PSRAM writes in the render, which
are bus-bound — a whole-frame estimate lands nearer ~80ms than 94/1.5. Cost is power draw; see the
battery measurement note in the backlog before treating this as settled.

Boot now prints the *measured* CPU frequency (`getCpuFrequencyMhz()` via `esp_clk_tree`), because
the only reason a 1.5× regression survived for months is that nothing ever printed it.

*Sensor rate 5 → 10Hz.* `SYSTEM_UPDATE_MS` 200 → 100. The System Task tick is the sensor clock: the
compass sub-timer (20ms gate) and the GPS gate (`GPS_UPDATE_INTERVAL_MS = 100`) both fire every
tick, so this doubles both. 10Hz is also the BH-880's native NAV-PVT rate, so GPS samples stop being
discarded, and `HEADING_SMOOTHING = 0.3f` was already tuned for a 10Hz compass it had never actually
received. Safe only because render requests are coalesced to at most one per UI Task loop.

- **Heading render deadband 1.5° → 0.5°** (`HEADING_RENDER_DEADBAND_DEG`). At 10Hz with EMA α=0.3 a
  single-sample noise excursion smooths to ~0.6°, so 0.5° still sits at the noise floor while a
  genuine 5°/s turn now redraws — under 1.5° nothing slower than 15°/s did. The threshold is close
  to vestigial as a load control either way: with a GPS fix, `RADAR_REFRESH` is queued every sample
  regardless, so it only gates anything indoors
- **Battery sampling explicitly pinned to 5Hz** in `systemTask`. `battery::update()` busy-waits
  ~1.5ms (15 ADC samples × 100µs) with no internal rate limit and was the only per-tick cost in that
  loop not already time-gated; everything downstream of it is 30s-gated, so doubling it bought
  nothing
- **The predicted input regression did not happen.** Verified outdoors with satellites locked:
  rotation and button both good. The concern was that with a fix, `RADAR_REFRESH` is queued every GPS
  sample, so the UI Task renders nearly every loop and polls input once per ~90ms instead of once per
  26.6ms vsync. That is still true — but ~90ms is comfortably shorter than a real button press (the
  boot log shows 132–186ms press durations), so nothing is missed and the latency stays under the
  perceptual threshold for a discrete action. The prediction conflated *poll interval* with *perceived
  latency*; for discrete input they are not the same thing.
- **Noted for a possible future revisit** (not implemented, not a user-facing setting): if Core 1 ever
  needs relief — after raising PCLK, or under a much heavier waypoint load — dropping the *GPS-driven*
  `RADAR_REFRESH` to 5Hz while leaving the compass at 10Hz would halve the render rate cheaply, since
  translation matters less than rotation. The compass rate is what makes the rotation feel right and
  should not be the thing lowered

**Fixed: committed `sdkconfig.cc-radar` had drifted from `sdkconfig.defaults`**

PlatformIO does not regenerate `sdkconfig.<env>` when `sdkconfig.defaults` changes — the first
240MHz build succeeded and still ran at 160MHz. Deleting the generated file and rebuilding revealed
three further settings that `sdkconfig.defaults` asked for and never got:

- `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` was **off** despite being requested — OTA rollback
  protection was configured but not armed. Now on; safe because `main.cpp:395` calls
  `esp_ota_mark_app_valid_cancel_rollback()`
- `CONFIG_PARTITION_TABLE_CUSTOM_FILENAME` still named the pre-OTA `partitions.csv`. Cosmetic only —
  PlatformIO's `board_build.partitions` governs the real table, and the built `partitions.bin` was
  verified to be the ota_0/ota_1 layout both before and after
- `CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL` existed **only** in the generated file, so regenerating
  reverted NimBLE allocations to IDF's INTERNAL default and would have pushed the beacon stack into
  the scarce internal SRAM. Now pinned in `sdkconfig.defaults`

**Radar frame 149ms → 94ms (~6.7 → ~10 fps): transpose tuning, then a zero-copy flush**

Steps 9b and 9 from the optimization backlog. Verified on hardware — no tearing, taps intact.

*Step 9b — transpose tuning.* `rotate90_tiled` gained `IRAM_ATTR` and an internal-SRAM scratch tile.
Tiling alone still left one of the two PSRAM streams strided: whichever loop is innermost gets
sequential access and the other hops by a 960-byte row stride. Transposing *via* a 2 KB SRAM tile
makes both PSRAM sides sequential — read a source row run, scatter into SRAM (uncached, free), emit
each destination row run as one `memcpy`. Rotation **64.1 → 55.7ms**.

Short of the ~40ms projected, and the reason was already sitting in the measurements: the flush
moved the same 460 KB PSRAM→PSRAM with an optimized `memcpy` in 34ms ≈ 27 MB/s, while the transpose
now runs at ~16.5 MB/s. Real headroom was ~1.6×, not the 1.6× *on top of* tiling that was assumed.

*Step 9 — `num_fbs = 2`, transpose straight into the back framebuffer.* The panel now allocates two
framebuffers. `rotate90_tiled` writes into the back one and hands that pointer to
`esp_lcd_panel_draw_bitmap`, which recognises its own framebuffer and swaps `cur_fb_index` instead
of copying (`esp_lcd_panel_rgb.c:614-624`). Flush **34.0 → 0.02ms** — deleted, not reduced.

Rotation *also* dropped **55.7 → 47.4ms**: the flush memcpy had been competing with the transpose
for PSRAM bandwidth and cache. That interaction reversed a step-9b decision — a 64-pixel tile beat
32 by 3.4ms while the flush existed, and by 0.5ms (noise) once it was gone, so the tile went back to
32 and kept 6 KB of SRAM.

- **Frame 149.6 → 94–101ms** depending on HUD content; rotation is now 50% of what remains
- **PSRAM −460 KB net**: +460 KB for the second framebuffer, −920 KB from no longer allocating the
  two rotation staging buffers at all
- **RAM +2,064 bytes** (the scratch tile), **Flash +872 bytes**
- `full_refresh = 1` is now load-bearing for the zero-copy path — a partial flush area would leave
  the rest of the alternate framebuffer holding a two-frames-old image. It moves in lockstep with
  the rotation mode in both init and the runtime `rot` switch, since LVGL rejects `full_refresh`
  together with `sw_rotate`
- Added an `on_frame_buf_complete` guard before the transpose: the driver latches
  `bb_fb_index = cur_fb_index` only at a frame boundary, so between a swap and that latch the back
  buffer is still being scanned out. At 94ms/frame vs a 26.6ms panel period it never blocks — it is
  there so step 10 (higher PCLK) cannot silently reintroduce tearing

**Radar frame 238ms → 149ms: dropped the canvas, then found two hidden full-screen repaints**

Two changes, one of which was found only because the other forced better instrumentation.

*Step 8 — drop the radar canvas (`816b421`).* The radar painted into a full-screen `lv_canvas`,
which LVGL treated as an image and blitted into the draw buffer on every refresh. Replaced with a
plain `lv_obj` that paints itself from an `LV_EVENT_DRAW_MAIN` handler, emitting geometry straight
into LVGL's draw context. Frame **238 → 210ms**, plus **460 KB of PSRAM freed**.

Far less than the ~104ms projected. The projection assumed the whole un-instrumented refresh
remainder (`refr − rot − flush`) was the canvas blit; it was not. The blit was worth ~22ms.

*Step 8b — `clip_corner` was defeating LVGL's cover-check (`44f6d0d`).* Bracketing the background
fill with a `DRAW_MAIN_BEGIN` timestamp split the remaining 82ms into `bg 23ms` + `non-radar 62ms`.
That 62ms was the screen background and the stage background being painted every frame — both
full-screen, both opaque, both the same green, both immediately covered by the radar.

Cause: `lv_obj_set_style_clip_corner(stage, true)`. LVGL answers `LV_EVENT_COVER_CHECK` with
`LV_COVER_RES_MASKED` for any object with `clip_corner` set, and `lv_refr_get_top_obj` treats
`MASKED` as *stop, do not descend into children*. The search for the topmost fully-covering object
bailed at the stage and never reached the radar, so LVGL drew from the screen down.

`clip_corner` also installs a radius mask that every child draw call blends through — which is what
made grid drawing 3× more expensive after step 8 moved painting inside the stage. One flag, both
symptoms. Frame **210 → 149ms**; grid **20–26 → 6–9ms**.

- **~0.8 fps → ~6.7 fps** across the full effort (frame ~499ms → 149ms)
- Nothing lost visually — the panel is physically round, so the clipped corners are not on the glass
- Timing semantics changed: painting now happens *inside* the LVGL refresh, so `paint` is a
  component of `refr`, not sequential with it. Frame = `label + refresh`. `perf`, the DEV tab and
  the on-screen HUD updated to match; `flush_us` added to attribute `esp_lcd_panel_draw_bitmap`
- Remaining: rotate 64ms / flush 34ms / bg 21.5ms / non-radar 16.4ms / paint 13ms
- Build: RAM 59.1% (193,520 B), Flash 79.4% (1,664,747 B)
- Files: `src/ui/ui_manager.cpp`, `src/ui/navigation.cpp`, `include/ui/navigation.h`,
  `include/ui/ui_manager.h`, `src/core/device_manager.cpp`, `src/utils/diagnostics.cpp`,
  `src/ui/dev_screen.cpp`, `docs/performance_optimization_backlog.md`

### Fixed

**Waypoint taps stopped opening the detail screen (regression from `816b421`)**

`lv_obj_create()` sets `LV_OBJ_FLAG_CLICKABLE` (`lv_obj.c:436`) where `lv_canvas_create()` does not.
Replacing the radar canvas with a plain object silently made the radar surface the hit-test winner
for every touch, so presses stopped at it instead of reaching the stage handler that calls
`handleTapAt()`. The radar rendered identically either way, so this was invisible until a waypoint
was tapped. Fixed by clearing the flag — the surface is for painting, input belongs to the stage
beneath it. Fixed in `44f6d0d`, verified on hardware.

**Brightness could be set to 0% with no way to recover**

`MIN_BRIGHTNESS_PERCENT = 5` was defined in `system_config.h` but referenced nowhere. The only
protection was the settings slider's range, which does not cover the NVS restore path at boot: a
stored raw level of 1–2 passes the `> 0` guard and divides down to 0%, leaving the panel dark with
the only control on a screen no longer visible. Moved the floor into `backlight::setPercent()` — the
single point every caller passes through — and switched standby to `backlight::off()`, which
bypasses it so deliberate full-off still works. Fixed in `aa66982`, verified on hardware.

**Radar rotation dropped to 1 Hz whenever GPS acquired a fix**

Heading rotation felt smooth while searching for satellites, then became visibly choppy the moment a
fix appeared. The renderer was not getting slower — the render *rate* dropped 5×.

`processUIUpdate()` handled `COMPASS_UPDATE` by skipping the redraw whenever GPS was valid, on the
stated reasoning that "`RADAR_REFRESH` is queued in the same burst". Both events are produced by the
System Task, but at different rates: the compass read is gated to 20ms so it fires every 200ms tick
(5 Hz), while the GPS read is gated by `GPS_UPDATE_INTERVAL_MS = 1000` (1 Hz). The bursts coincide 1
time in 5. The other 4 compass updates advanced `ui.current_heading` and drew nothing, so rotation
was pinned to the 1 Hz GPS rate and each frame showed a heading up to a second stale.

Fixed by coalescing instead of suppressing. All four render-triggering cases (`RADAR_REFRESH`,
`COMPASS_UPDATE`, `ZOOM_CHANGE`, `ZOOM_CHANGE_REVERSE`) now call `requestRadarRender()`, which only
sets a flag; the UI Task calls `flushRadarRender()` once after draining the queue batch, still inside
`display_mutex`, with a standby guard so a queued refresh cannot paint a screen that is off.

- Rotation now tracks the fastest producer: **1 Hz → 5 Hz with a fix**
- Renders per UI Task loop capped at **1** (was up to 4) — strictly fewer worst-case renders than
  before, and resolves §3.3 of the perf backlog, which warned of 4 × 149ms with the mutex held
- Verified on hardware with a 14-satellite fix: rotation smooth, button remains responsive
- This was §3.2 of the perf backlog, ranked step 11 behind four large pipeline rewrites. It needed
  none of them — the preceding `fill_bg` fix had already made frames cheap enough
- Build: RAM 59.0% (193,424 B), Flash 79.3% (1,662,635 B)
- Files: `src/utils/task_manager.cpp`, `docs/performance_optimization_backlog.md`, `ROADMAP.md`

**Radar frame time: canvas clear was 59% of every frame (205ms → 21ms)**

`updateRadarDisplay()` cleared the 480×480 radar canvas with `lv_canvas_fill_bg()`. For
`LV_IMG_CF_TRUE_COLOR` that LVGL function takes a per-pixel path — `lv_img_buf_set_px_color()` plus
`lv_img_buf_set_px_alpha()` for every pixel, i.e. **460,800 out-of-line calls per frame**, each doing a
colour-format switch and pointer arithmetic into PSRAM. Measured at 205ms, an effective 2.25 MB/s.

The radar canvas has no alpha channel, so `set_px_alpha` was a no-op on every one of those calls.
Replaced with `lv_color_fill()` over `dsc->data`, which is semantically identical for this format.

- `fill_bg` 205.0ms → 21.3ms (9.6×); paint stage 215.3ms → 32.2ms (6.7×)
- Frame time at unchanged rotation: ~499ms → 316ms
- Found by measurement, not inspection — two prior hypotheses (both blaming software rotation) were wrong
- Follow-up measured: software rotation costs ~153ms/frame; base refresh blit ~131ms
- Files: `src/ui/navigation.cpp`, `docs/performance_optimization_backlog.md`

### Added

**DEV Render Timing HUD**

On-screen render instrumentation for the performance work tracked in `docs/performance_optimization_backlog.md`. Splits a radar frame into its two measurable halves so the cost of software rotation can be attributed rather than guessed:

- `paint` — time inside `updateRadarDisplay()` (canvas painting), microsecond resolution
- `refr` — LVGL blit + 90° software rotate + flush dispatch, via `disp_drv.monitor_cb`
- `total` and `fps` — FPS measured from real panel flushes (`NavState::flush_count`) over a 1s window

Visible on the radar screen when dev mode is on; follows the same show/hide path as the DEV label (boot, `showHUD`/`hideHUD`, and runtime `DEV_MODE_CHANGE`).

- Positioned `LV_ALIGN_CENTER, 0, 120` — absolute corners are clipped by the round bezel
- Files: `include/ui/navigation.h`, `src/ui/navigation.cpp`, `src/core/device_manager.cpp`, `src/ui/ui_manager.cpp`, `include/ui/ui_manager.h`, `src/utils/task_manager.cpp`

**Build-time rotation toggle for A/B testing**

`-DRADAR_ROTATION_DEGREES=0` disables software rotation so the cost of `sw_rotate` can be measured directly. UI renders sideways — intended for measurement only.

- Files: `include/core/system_config.h`

### Changed

**Disabled LVGL built-in perf monitor** (`LV_USE_PERF_MONITOR 0`)

It was enabled but aligned to `LV_ALIGN_BOTTOM_RIGHT`, which sits behind the bezel on this round 480×480 panel — never visible, while still costing a label refresh every 300ms. Replaced by the DEV render timing HUD above.

- Build impact: flash 1,660,864 → 1,660,803 bytes (−61), RAM unchanged at 193,408 bytes
- Files: `include/ui/lv_conf.h`

---

## [Beta] - 2026-04-13

### Added

**OTA Firmware Update via Web Browser**

Full over-the-air firmware update system accessible from the GPX web portal at `/update`. The device writes the new binary directly to the inactive OTA partition, sets the boot partition, and reboots. A one-shot server guard (`ota_already_triggered`) prevents a browser retry or stale tab from re-flashing the device after a successful update. The OTA page matches the dark monospace aesthetic of the rest of the portal.

- Dual OTA partition table (`partitions_ota.csv`): 2×2MB app slots + 11.7MB FFat
- `esp_ota_mark_app_valid_cancel_rollback()` called on successful boot (rollback safety)
- `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` added to `sdkconfig.defaults`
- Warning message on upload page: display interference during flash is expected
- Files: `src/gpx/gpx_server.cpp`, `partitions/partitions_ota.csv`, `sdkconfig.defaults`

**CalVer Versioning + Per-Build Stamp**

Build version `vYY.MM.DD` written to `include/core/fw_version_gen.h` by `scripts/gen_version.py` (PlatformIO pre-build extra_script). A Unix build timestamp `FW_BUILD_TS` is also written on every build, ensuring `settings_manager.cpp` always recompiles and `FW_STAMP_VAL` is unique per build.

- Version displayed on loading screen (radar green, `LV_ALIGN_BOTTOM_MID`)
- `version` serial command added to diagnostics
- Files: `scripts/gen_version.py`, `include/core/fw_version_gen.h`, `src/utils/settings_manager.cpp`

**Radar-Mode Boot Guarantee After Any Firmware Flash**

First boot after any firmware update (USB or OTA) now always lands in radar mode, regardless of previous WiFi settings.

- Root cause: `FW_STAMP_VAL` was `fnv1a(FW_VERSION)` — same-day builds produced identical stamps, NVS WiFi flags survived any same-day flash
- Fix: `FW_STAMP_VAL = FW_BUILD_TS` (Unix timestamp, unique per build). Any mismatch → WiFi boot flags cleared → radar mode
- OTA handler calls `settings_manager::prepareForOTAReboot()`: writes `fw_stamp=0` + clears `wifi_ap_en`/`wifi_sta_en` in one NVS transaction — stamp=0 always mismatches any real `FW_BUILD_TS`

### Changed

**GPX Web Portal — Dark Monospace Theme**

Main GPX upload page (`/`) redesigned to match the OTA page aesthetic: `#1a1a1a` dark background, monospace font, `#00ff00` green accents, dashed drag-drop zone, dark file list. Removed purple gradient, emojis, and white card. All JavaScript functionality (drag-and-drop, file list, download, delete) unchanged.

---

## [Beta] - 2026-03-25

### Added

**Beacon Found Indicator**

A 40×40 LVGL canvas overlay (circle + star) is drawn at screen center when the beacon has been marked as found (NVS `bcn_found` flag set). The indicator renders in white on the dark theme and black on daylight mode, matches the existing DEV label position, and hides automatically when the HUD is hidden.

**Beacon 4-Zone Musical Tempo**

Sonar beep intervals restructured to four distinct zones: VERY_FAR 1500ms, FAR 750ms, MEDIUM 500ms, CLOSE 250ms. EMA smoothing (α=0.4), ±3 dBm hysteresis between zone transitions, and trend detection over 10 samples prevent zone flicker. Replaces the previous linear interval mapping that produced choppy audio at close range (FT-04 resolved).

**Fixed Waypoint UX Improvements**

Proximity star drawn on the waypoint dot itself at three sizes scaled to zone distance. Waypoint label renamed to "Fixed:" with position tuned for safe screen margin. Auto-unfix triggers when distance exceeds 1km. North indicator is hidden when north-up mode is active (not needed when north is always up).

### Changed

**Beacon Scan Interval**

BLE scan interval reduced from 1000ms to 500ms, improving RSSI update responsiveness when near the target beacon.

**Beacon Found = Silent**

`updateSonar()` returns immediately when `g_found == true`. Prevents a NVS-persisted found state (from a previous session) from triggering audio at startup before the user resets the found flag.

**DEV Label Position**

DEV mode overlay label moved to screen center to avoid collision with other HUD elements at screen edges.

### Fixed

**WiFi AP Chunked HTTP Responses**

GPX management web page responses now sent in 1KB chunks via `httpd_resp_send_chunk()`. Fixes EAGAIN errors on the ESP32 soft-AP TCP buffer that caused partial page loads or browser timeouts when serving the file management UI.

---

## [Unreleased] - 2026-03-20

### Changed

**SRAM Optimization — NimBLE Migration**

Replaced Bluedroid BLE stack with NimBLE (`h2zero/NimBLE-Arduino@^1.4.0`) to resolve critical SRAM exhaustion caused by the Bluedroid stack consuming ~65KB at runtime.

**Root cause**: After BLE init, only ~2–7KB SRAM remained — not enough for SD card DMA buffers (~4–16KB). Every SD log flush failed with `sdmmc_read_blocks failed (257)`. The same low-memory condition caused LVGL heap fragmentation stalls, making double-press detection unreliable.

**Result**: NimBLE uses ~25KB SRAM (vs ~65KB Bluedroid), freeing ~40KB.

**Symptoms fixed**:
- ✅ SD card write failures every 30s — eliminated
- ✅ Double-press detection restored (heap stalls gone)
- ✅ ~40KB SRAM headroom when BLE active (was ~2–7KB)

**API changes** in `beacon_proximity.cpp`:
- Single `#include <NimBLEDevice.h>` replaces three Bluedroid headers
- `BLEDevice::*` → `NimBLEDevice::*`
- `BLEAdvertisedDeviceCallbacks` → `NimBLEAdvertisedDeviceCallbacks`
- `onResult(BLEAdvertisedDevice)` → `onResult(NimBLEAdvertisedDevice*)`
- Scan completion: timer-based → `g_pScan->isScanning()` (more reliable)
- SRAM guard threshold lowered: 60KB → 30KB
- All business logic (EMA, zones, hysteresis, trend, sonar) unchanged

**Files**:
- `platformio.ini` — added `h2zero/NimBLE-Arduino@^1.4.0`
- `src/hardware/connectivity/beacon_proximity.cpp` — NimBLE API rewrite

**Build impact**: −8,184 bytes static RAM (RAM: 59.3% / 194,152 bytes)

---

**SRAM Optimization — System Logger Heap Allocation**

System logger `g_buffer` (8KB) was a static `.bss` allocation — always in SRAM, even when logging was disabled.

**Fix**:
- `g_buffer` converted from `static char[8192]` to heap-allocated `char*` in `init()`
- `init()` only called when `settings.logging_enabled == true`
- `close()` frees the buffer
- **Normal mode: 0 bytes used** by the logger (no buffer, no mutex, no SD overhead)
- DEV mode with logging on: 8KB allocated from heap on demand

**Files**:
- `src/utils/system_logger.cpp` — heap allocation/free
- `src/core/main.cpp` — init gated behind `settings.logging_enabled`

**Build impact**: −8,184 bytes static RAM (included in NimBLE total above)

---

**SRAM Optimization — NTP Sync Removal**

WiFi NTP was running a 10-second polling loop in `loop()` even when WiFi was fully disabled. RTC (PCF85063) and GPS time sync via `task_manager::queueGPSTimeSync` provide all needed accuracy.

**Fix**: NTP polling timer removed from `loop()`. `ntp_sync::init(false)` disables auto-sync at boot.

**Files**: `src/core/main.cpp`

---

**LVGL Full-Frame Buffers (480 lines)**

Upgraded LVGL draw buffers from 160 lines to 480 lines (full frame). Eliminates the visible top-to-bottom screen wipe artifact that appeared during transitions when LVGL software rotation is active (the rotation requires a temporary buffer that fits cleanly in the 64KB `LV_MEM_SIZE`).

**Details**:
- Full frame: 480 × 480 × 2 bytes × 2 buffers = **921 KB PSRAM**
- 1 flush per frame (was 3 at 160 lines)
- `BUFFER_LINES = 480` in `include/core/system_config.h`

**Files**: `include/core/system_config.h`

---

### Fixed

**DEV Mode No Longer Forces Logging On**

DEV mode was calling `settings_manager::saveLoggingEnabled(true)` during boot, overwriting the user's `logging: OFF` NVS setting on every restart. User had to re-disable logging after every reboot.

**Fix**: Removed auto-enable block. Logging state is fully user-controlled.

**Files**: `src/core/main.cpp`

---

**WiFi Boot Settings Not Applied to Scanner**

`scanner.cpp` had a hardcoded `wifi_enabled = true` that was never synced from NVS. WiFi scanning ran at boot even with WiFi disabled in Settings, producing spurious scan errors in serial output.

**Fix**: Added `scanner::setWiFiEnabled(settings.wifi_enabled)` at boot alongside `wifi_manager::setEnabled()`.

**Files**: `src/core/main.cpp`

---

**GPX Description Truncation**

Three internal `tmp[]` buffers in `gpx_loader.cpp` were 192–256 bytes — too small for geocaching long-form descriptions, causing mid-sentence cuts in the waypoint detail screen.

**Fix**:
- All three buffers increased to 512 bytes (matches the line read buffer size)
- Added "..." truncation at last sentence boundary when the 1024-byte description buffer fills

**Files**: `src/gpx/gpx_loader.cpp`

---

**Waypoint Detail Screen — HINT Section Unreachable**

Long descriptions pushed the HINT section below the reachable scroll area, making it inaccessible even by scrolling to the bottom.

**Fix**: HINT section placed before DESCRIPTION in the flex column layout.
- HINT shows a tap-to-reveal header, hidden text by default
- DESCRIPTION scrolls below
- User can always reach HINT at the top of the scroll view

**Files**: `src/ui/waypoint_screen.cpp`

---

### Added

**Beitian BH-880 GPS+Compass Module**

Replaced Quectel LC76G GPS module with Beitian BH-880 (B1301N GPS chip + QMC5883L compass). Drop-in replacement on same UART pins (GPIO43/44, 115200 baud).

**GPS improvements**:
- 10Hz update rate (was 1Hz)
- Multi-constellation: GPS, GLONASS, BDS, Galileo, IRNSS, SBAS, QZSS (120 channels)
- Sensitivity: −163 dBm tracking, −148 dBm cold start
- Cold start: 28s, hot start: 1s

**Critical power note**: Module requires 3.6–5.5V. The board 3.3V rail is insufficient for a GPS fix. Solution: power VCC directly from LiPo positive (3.7–4.2V). QMC5883L compass rated 2.16–3.6V — works fine on 3.3V.

**Files**:
- `include/hardware/sensors/gps_bh880.h` / `src/hardware/sensors/gps_bh880.cpp`

---

**QMC5883L Compass System**

Replaced IMU/Gyro heading fusion with the QMC5883L magnetometer built into the BH-880 module. Compass is the sole heading source — GPS heading fusion removed.

**Architecture**:
- System Task reads QMC5883L every ~1s → queues `COMPASS_UPDATE` → UI Task applies to `ui.current_heading`
- All waypoints, off-screen indicators, and north indicator rotate from `ui.current_heading`
- Reaction time: ~1s. Smooth for walking speed.

**Critical I2C constraint**: Compass cannot be read from the I2C Task. The CST820 touch driver calls `Wire.requestFrom()` directly, bypassing `i2c_mutex`. Reading compass from I2C Task causes immediate `Wire.cpp requestFrom Error -1`. Must use System Task (tolerates occasional errors).

**Reference**: `docs/compass_i2c_constraint.md`

**Files**:
- `include/hardware/sensors/compass_qmc5883l.h` / `src/hardware/sensors/compass_qmc5883l.cpp`
- `src/utils/task_manager.cpp` — System Task reads + `COMPASS_UPDATE` queue message

---

**WMM Magnetic Declination**

Auto-computes magnetic declination from GPS fix using a truncated WMM2020 spherical harmonic model (n=1..3, ±1° accuracy). Applied to every compass reading to produce true heading.

**Behavior**:
- Computed once per session at first valid GPS fix
- Persisted to NVS, reused on subsequent boots until a new fix updates it
- Sign convention: `true_heading += declination` (positive = East declination)
- Example: Los Angeles area ~12.25° East at (34.13°N, 118.15°W) in 2026.2

**Files**:
- `include/utils/wmm_declination.h` / `src/utils/wmm_declination.cpp`

---

**Build: NimBLE Library**

Added `h2zero/NimBLE-Arduino@^1.4.0` to `platformio.ini` lib_deps.

---

## [v0.14.0] - 2026-01-30

### Added

**Beacon Proximity System**

BLE-based item finder that turns the radar into a proximity detector. When zoomed to 50m, the device scans for a configured BLE beacon MAC address and provides real-time visual + audio feedback.

**Visual Arc Gauge**:
- Cyan arc drawn clockwise around the radar circle outer edge
- Fills from 0° (no signal, -90 dBm) to 355° (full circle, -45 dBm) based on EMA-smoothed RSSI
- 14px line width at 228px radius — clearly visible without obscuring radar content
- Minimum 10° arc when any signal detected (always visible when in range)

**Audio Sonar Beeping**:
- Buzzer pulses at 1800ms (far) → 900ms → 500ms → 200ms (< 1m) as signal strengthens
- Can be independently disabled in Settings > Sound > Beacon Sound toggle
- Non-blocking state machine — zero impact on UI responsiveness

**RSSI Processing**:
- EMA smoothing (α = 0.4) prevents visual/audio jitter from signal fluctuations
- Zone-based detection (OUT_OF_RANGE / FAR / MEDIUM / CLOSE) with ±3 dBm hysteresis
- Requires 2 consecutive readings to confirm zone change (prevents oscillation)
- Linear trend detection over last 10 samples: APPROACHING / DEPARTING / STABLE

**Integration**:
- **Zoom-gated**: Only activates at 50m zoom — stops automatically when zooming out
- **BLE scanning**: 1s scan every 2s (50% duty cycle), early-exit when target MAC found
- **15s timeout**: Beacon marked lost if not seen for 15 seconds

**Settings (NVS persistent)**:
- Target MAC address, measured power (dBm @ 1m), path loss exponent
- Separate toggle for sound vs visual
- Keys: `bcn_en`, `bcn_snd`, `bcn_mac`, `bcn_pwr`, `bcn_n`

**Build Impact**: ~3,500 bytes flash, ~2KB RAM

**Files**:
- `include/hardware/connectivity/beacon_proximity.h` - API and state structures
- `src/hardware/connectivity/beacon_proximity.cpp` - BLE scanning, EMA, zone logic
- `src/ui/navigation.cpp:390-420` - `drawBeaconProximityGauge()` arc drawing
- `src/utils/task_manager.cpp:79-94` - Zoom-gating activation logic
- `src/utils/diagnostics.cpp:1255-1403` - Serial diagnostic commands
- `src/ui/settings_screen.cpp:1241-1310` - Beacon Sound settings toggle
- `src/utils/settings_manager.cpp:656-705` - NVS persistence

**Serial Commands**: `beacon status`, `beacon scan`, `beacon test`, `beacon zone`, `beacon trend`, `beacon debug`, `beacon reset`, `beacon mac/power/n`

**Reference Documentation**: [`docs/beacon_proximity.md`](docs/beacon_proximity.md)

---

**Gyro Calibration System (Calib Tab)**

New dedicated Calibration tab in Settings for gyro-based heading fusion. Provides smooth heading tracking when GPS is unreliable (stationary or slow walking).

**Features:**
- **Calib Tab**: Always visible in Settings (between Sound and DEV)
- **Two UI States**: DISABLED / RUNNING (calibrated) or NEEDS CALIBRATION
- **50m Zoom Activation**: Gyro only runs at 50m zoom to save resources
- **Persistent Calibration**: Saved to NVS, survives reboots indefinitely
- **One-time Calibration**: Calibrate once, valid for months/years
- **UI Feedback**: Shows "CALIBRATING..." during calibration process

**User Workflow:**
1. Enable toggle in Settings > Calib
2. Press Calibrate button (keep device still for ~5 seconds)
3. Status changes to "RUNNING" with "Calibrated: YES (saved)"
4. Gyro auto-activates when zooming to 50m for precision navigation

**Key Files:**
- `src/ui/settings_screen.cpp:2181-2400` - Calib tab UI and status display
- `src/utils/task_manager.cpp:69-106` - Zoom-based gyro activation
- `src/navigation/imu_sampling.cpp` - 100Hz gyro sampling and calibration
- `src/utils/settings_manager.cpp:669-703` - NVS persistence

**Technical Details:**
- QMI8658 IMU at 100Hz sampling rate
- Calibration requires 500 samples (~5 seconds)
- Bias values stored in NVS (imu_cal, imu_bx, imu_by, imu_bz)
- Heap allocation for calibration buffer (avoids stack overflow)

**Build Impact**: +500 bytes flash (minimal)

---

### Fixed

**CRITICAL: UI_Task Freeze After Extended Runtime (Priority Alpha)**

Fixed a thread-safety bug that caused the UI_Task to freeze after 7+ hours of runtime. The freeze was caused by unsafe LVGL calls from button callbacks during standby enter/wake operations.

**Root Cause Analysis:**
- `enterStandby()` and `wakeFromStandby()` were called directly from button callbacks
- Button callbacks run during `button::update()` which is OUTSIDE the display_mutex
- Both functions made LVGL calls (lv_obj_create, lv_timer_create, etc.) without mutex protection
- `wakeFromStandby()` also called `lv_timer_handler()` directly - potentially recursive/concurrent
- Over time, this corrupted LVGL internal state, causing UI_Task to hang

**Solution:**
1. Added `ENTER_STANDBY` and `WAKE_STANDBY` to UIUpdateType enum
2. Button callbacks now queue standby operations instead of direct calls
3. Standby operations are processed inside display_mutex (thread-safe)
4. Removed dangerous `lv_timer_handler()` call from `wakeFromStandby()`

**Key Changes:**
- `include/utils/task_manager.h:96-97` - Added ENTER_STANDBY, WAKE_STANDBY enum values
- `src/core/device_manager.cpp:635-696` - Queue standby ops instead of direct calls
- `src/utils/task_manager.cpp:548-560` - Handle ENTER_STANDBY/WAKE_STANDBY in processUIUpdate()
- `src/utils/standby_manager.cpp:126` - Removed unsafe lv_timer_handler() call

**Symptoms Fixed:**
- UI_Task becomes UNRECOVERABLE after ~452 minutes (7.5 hours)
- Button presses stop working (zoom, settings, etc.)
- Position stops updating on radar display
- Recovery attempts (suspend/resume) fail

**Build Impact**: +352 bytes flash (minimal)

### Added

**Button Sound Diagnostic Feature (Priority 2.11 Phase 1)**

Implemented basic buzzer functionality for diagnosing UI freeze issues. When enabled, a short chirp plays on button press, helping determine if the button hardware works when the UI becomes unresponsive.

**New Files**:
- `include/hardware/buzzer.h` - Buzzer interface
- `src/hardware/buzzer.cpp` - Buzzer implementation using TCA9554 EXIO pin 7

**Settings Changes**:
- New "Sound" tab in Settings (between Display and DEV)
- "Button Sound" toggle (default: OFF)
- "Test Beep" button for testing buzzer

**Settings Storage**:
- New NVS key `btn_sound` for button sound preference
- New `button_sound_enabled` field in RadarSettings

**Key Changes**:
- `src/hardware/input/button.cpp:118-131` - Plays chirp on button press when enabled
- `src/core/device_manager.cpp:698-703` - Buzzer initialization after button init
- `src/core/device_manager.cpp:707` - Buzzer update for timing management
- `src/ui/settings_screen.cpp:1262-1370` - Sound tab implementation
- `include/ui/ui_manager.h:119` - Added settings_tab_sound

**Build Impact**: +1.5KB flash (minimal)

---

## [v0.13.0] - 2026-01-21

### Added

**5-Phase Stability Overhaul - Thread-Safe UI Architecture**

Complete system stability rewrite to fix crashes caused by thread-unsafe LVGL access from button callbacks. The system now uses queue-based UI updates, mutex protection, hardware watchdog, enhanced crash logging, and task health monitoring.

**Phase 1: Queue-Based UI Updates**
- Button callbacks now queue UI requests instead of direct LVGL calls
- New UIUpdateType enum values: `ZOOM_CHANGE`, `ZOOM_CHANGE_REVERSE`, `SETTINGS_SCREEN`
- `processUIUpdate()` handles all UI operations safely within UI Task context
- Eliminates race condition between button ISR and LVGL timer handler

**Implementation**:
```cpp
// BEFORE (unsafe - caused crashes):
ui.cycleZoom();
navigation::updateRadarDisplay();

// AFTER (safe - queued):
UIUpdate update;
update.type = UIUpdateType::ZOOM_CHANGE;
task_manager::queueUIUpdate(update);
```

**Phase 2: Mutex Protection**
- `display_mutex` wraps `lv_timer_handler()` and UI queue processing
- `ui_state_mutex` protects UIState field access
- Thread-safe accessor functions: `getCurrentZoomLevel()`, `cycleZoomForward()`, `cycleZoomBackward()`
- `withDisplayMutex()` helper for safe LVGL operations from any context

**Phase 3: ESP32 Task Watchdog (TWDT)**
- Hardware-level detection of hung tasks
- 30-second timeout with warning (no panic by default)
- All tasks subscribe and feed watchdog every loop iteration
- ESP-IDF version compatibility (4.x and 5.x API support)
- New files: `include/utils/watchdog.h`, `src/utils/watchdog.cpp`

**Phase 4: Enhanced Crash Logging**
- `CrashInfo` structure with RTC memory persistence (survives reboot)
- `captureState()` records heap, PSRAM, task loops, last operation
- `logBootReason()` logs ESP32 reset reason on every boot
- `SYSLOG_CHECKPOINT()` macro for strategic crash location tracking
- Boot reason detection: power-on, software reset, brownout, panic, etc.

**Phase 5: Task Health Monitoring**
- `TaskHealth` structure tracks per-task health metrics
- `last_loop_time_ms` recorded every task iteration
- 5-second unresponsive threshold triggers recovery attempt
- `attemptTaskRecovery()` suspends/resumes hung tasks (max 3 attempts)
- Recovery logging with task identification

**Build Impact**: +4,892 bytes flash (1,402,149 bytes total, 44.6%), +48 bytes RAM

**Files Modified**:
- `include/utils/task_manager.h` - UIUpdateType enum, TaskHealth struct
- `src/utils/task_manager.cpp` - Queue processing, mutex, watchdog, health monitoring
- `src/core/device_manager.cpp` - Button callback uses queue instead of direct LVGL
- `include/utils/watchdog.h` - NEW: ESP32 TWDT wrapper API
- `src/utils/watchdog.cpp` - NEW: TWDT implementation with version compatibility
- `include/utils/system_logger.h` - CrashInfo struct, checkpoint macro
- `src/utils/system_logger.cpp` - RTC crash state, boot reason logging
- `src/core/main.cpp` - Watchdog initialization, boot reason logging

**Stability Improvements**:
- ✅ Zero crashes from rapid button presses (tested 10+ presses in 2s)
- ✅ No race conditions between button and UI tasks
- ✅ Hardware watchdog catches hung tasks before system freezes
- ✅ Crash state captured for post-mortem debugging
- ✅ Automatic task recovery attempts before giving up

---

**DEV Mode Enhancements**

Improved developer experience when DEV mode is enabled in settings.

**Auto-Enable Logging**:
- System logging automatically enabled when DEV mode is active
- Triggered on boot if `dev_tab_visible = true`
- Triggered when `dev show` command is used
- Serial output: `[DEV] Developer mode active - logging enabled`

**Power Management Override**:
- All automatic power management DISABLED when DEV mode is on
- No automatic brightness changes
- No automatic GPS preset changes
- No automatic standby entry
- Gives developer full manual control for testing
- Serial output: `[POWER] DEV MODE ACTIVE - All automatic power management DISABLED`

**Build Impact**: +156 bytes flash

**Files Modified**:
- `src/utils/power_manager.cpp` - Skip automatic power management when DEV mode enabled
- `src/utils/diagnostics.cpp` - Auto-enable logging on `dev show` command
- `src/core/main.cpp` - Auto-enable logging at boot when DEV mode already on

---

**Daylight Mode - High Contrast Outdoor Display**

New display mode optimized for outdoor visibility with bright sunlight.

**Color Scheme**:
| Element | Normal Mode | Daylight Mode |
|---------|-------------|---------------|
| Background | Dark green (`#3A9949`) | Light green (`#E0FFE0`) |
| Grid | Black | Black |
| Waypoints | Yellow with glow | Dark navy blue (`#000080`) |
| Center triangle | Red | Dark red |
| Glow effect | Enabled (soft yellow) | Disabled (no glow) |

**Shadow Overlay Adjustment**:
- Normal mode: 30% opacity (was 50%)
- Daylight mode: 0% opacity (invisible)
- Depth effect preserved in normal mode, maximum visibility in daylight

**HUD Labels with Background Containers**:
All HUD labels now have rounded background boxes for improved legibility:
- **Battery**: Dark green background (`#00AA00`), white text, 8px rounded corners
- **GPS Status**: Dark green background (`#006600`), white text
- **DEV Indicator**: Dark orange background (`#CC5500`), white text
- Backgrounds darken further in daylight mode for maximum contrast

**Settings Integration**:
- Toggle: Settings → Display → Daylight Mode
- NVS persistence: Setting saved across reboots
- Real-time switching: No restart required
- Description: "Use bright background for outdoor visibility"

**User Experience**:
- **Before**: Screen nearly invisible in direct sunlight
- **After**: High contrast colors readable outdoors
- **Result**: Usable navigation in all lighting conditions

**Build Impact**: +860 bytes flash, +16 bytes RAM

**Files Modified**:
- `include/settings_manager.h:40` - Added `daylight_mode = false` to RadarSettings
- `src/utils/settings_manager.cpp` - NVS load/save for daylight mode
- `include/ui/ui_manager.h` - Added shadow_overlay and HUD background references
- `src/ui/ui_manager.cpp` - HUD label backgrounds, updateDaylightMode() function
- `src/ui/navigation.cpp` - ColorScheme struct, getColorScheme(), light green daylight bg
- `src/ui/settings_screen.cpp:1201-1243` - Daylight Mode toggle in Display tab

**Brightness Verification**:
- Confirmed PWM at maximum (255 on 8-bit scale)
- Hardware limitation: Backlight is already at 100%
- Daylight mode compensates with high-contrast colors instead

---

## [v0.12.0] - 2025-10-26

### Added

**Independent Zoom Level Display**
- Zoom level now displayed separately from GPS status (always visible)
- Position: Bottom-center, above GPS status label
- Format: `[5km]` (in brackets for clear distinction)
- Updates independently when user changes zoom (single/double click)
- Works even when GPS is searching or disconnected

**User Experience**:
- **Before**: Zoom level only shown with GPS fix: "GPS: Fixed (8 sats) [100m]"
- **After**: Always visible: `[100m]` above "GPS: Searching..." or "GPS: Fixed (8 sats)"
- **Result**: User always knows current zoom level, even without GPS

**Visual Layout** (bottom-center):
```
        [5km]           ← Zoom level (always visible)
  GPS: Searching...     ← GPS status (changes color)
```

**Build Impact**: +148 bytes flash (1,394,769 bytes total, 44.3%), +16 bytes RAM

**Files Modified**:
- `include/ui/ui_manager.h:108` - Added `zoom_label` to UIState
- `src/ui/ui_manager.cpp:204-210` - Created zoom label above GPS status
- `src/ui/navigation.cpp:553-577` - Update zoom label independently from GPS status

---

**5km Zoom Level - Intermediate Regional View**
- New zoom level between 10km and 1km: **5km radius** with 1.25km grid spacing (60px)
- Zoom sequence: 10km → 5km → 1km → 500m → 100m → 10m (6 levels total)
- Provides better granularity for regional navigation (between city-wide and neighborhood views)
- Grid spacing follows progressive pattern: **48px → 60px → 80px → 96px → 120px → 160px** (squares get LARGER as you zoom in, all distinct)
- **User Feedback**: Grid squares visually distinguish each zoom level - 5km has medium squares between 10km (smallest) and 1km (larger)

**Center-Aligned Grid System**
- **All zoom levels** now have horizontal and vertical lines passing through screen center
- Creates smooth, natural zoom transitions - center crosshair remains anchored
- Grid spacing calculated to divide evenly into 240px (half-screen radius)
- Mathematical precision: Each zoom has exact integer number of grids per half-screen

**Grid Configuration** (480px screen width, all divisible for center line):
- **10km**: 48px spacing (10 lines total) ✓ Center line at 240px
- **5km**: 60px spacing (8 lines total) ✓ Center line at 240px
- **1km**: 80px spacing (6 lines total) ✓ Center line at 240px
- **500m**: **96px** spacing (5 lines total) ✓ Center line at 240px - **Distinct from 1km**
- **100m**: 120px spacing (4 lines total) ✓ Center line at 240px
- **10m**: **160px** spacing (3 lines total) ✓ Center line at 240px - **Distinct from 100m**

**User Experience**:
- **Before**: Some zooms had center lines, others didn't (felt "off-center")
- **After**: Every zoom has perfect center crosshair (horizontal + vertical lines)
- **Result**: Buttery smooth zoom transitions, user position always visually centered

**Waypoint Filtering Rules (Confirmed)**:
- **On-screen**: Waypoints within zoom radius (5km) shown as yellow circles
- **Off-screen arrows**: Waypoints within 10× radius (5-50km) shown as orange triangles
- **Filtered out**: Waypoints beyond 50km not displayed at 5km zoom
- **Same rule applies** to all zoom levels: 10km shows 0-10km on-screen, 10-100km as arrows

**Off-Screen Indicator Enhancement**:
- **Base width doubled** horizontally for better visibility at screen edges
- Height unchanged (maintains pointing direction clarity)
- Triangle now wider and more prominent without obscuring direction

**User Experience**:
- **Single Click**: Zoom in (10km → 5km → 1km → 500m → 100m → 10m → loops back to 10km)
- **Double Click**: Zoom out (reverse order: 10m → 100m → 500m → 1km → 5km → 10km)
- **Result**: More intuitive zoom control, better regional navigation granularity, improved waypoint visibility

**Build Impact**: +148 bytes flash (1,394,769 bytes total, 44.3%), +16 bytes RAM

**Files Modified**:
- `include/ui/ui_manager.h:27-34` - Added ZOOM_5KM enum, renumbered subsequent levels
- `include/ui/ui_manager.h:80-87` - All 6 zoom levels recalculated for center-aligned grids
- `src/ui/navigation.cpp:557-563` - Added "5km" label in zoom display switch
- `src/ui/navigation.cpp:374-398` - Doubled base width of off-screen indicator triangles
- `src/core/device_manager.cpp:590-603` - Changed double-click from resetZoom() to cycleZoomReverse()

### Changed

**Button Zoom Controls - Bidirectional Zoom**
- **Double-click behavior changed**: Now zooms OUT instead of resetting to default (100m)
- Single click: Zoom IN (10km → 5km → 1km → 500m → 100m → 10m)
- Double click: Zoom OUT (10m → 100m → 500m → 1km → 5km → 10km)
- Removed "reset to default zoom" feature (no longer needed with bidirectional control)

**User Experience**:
- **Before**: Single click zooms in, double click resets to 100m (always interrupts workflow)
- **After**: Single click zooms in, double click zooms out (smooth bidirectional control)
- **Result**: Natural zoom control without workflow interruption

**Build Impact**: No size change (function call replacement only)

**Files Modified**:
- `src/core/device_manager.cpp:591` - Serial log: "zooming out" instead of "resetting zoom to default"
- `src/core/device_manager.cpp:600` - Call `ui.cycleZoomReverse()` instead of `ui.resetZoom()`

---

**Standby Mode - Low-Power Sleep Function**
- GPIO0 4-second press enters standby mode (display OFF, GPS ON, WiFi/AP OFF)
- Any GPIO0 press wakes from standby, returning to previous screen
- Power consumption reduced from ~520mA (active) to ~55mA (standby) = 89% reduction
- Battery life: 5.8 hours active → 54 hours standby (3000mAh battery)
- Standby screen shows: "STANDBY MODE", battery %, time, "Press button to wake"
- 3-second transition screen before display turns OFF
- Statistics tracking: total standby count, total time, last duration

**User Experience**:
- **Entering Standby**: Hold GPIO0 for 4 seconds (2s more after Settings opens)
- **Standby Screen**: Black screen with white text, shows for 3 seconds
- **Display OFF**: Backlight fades to 0%, GPS continues tracking in background
- **Waking**: Press GPIO0 once, display turns ON, returns to exact same screen (Settings or Radar)
- **Result**: Long-term field use without draining battery, GPS track never interrupted

**Technical Architecture - Overlay Approach**:
- **Problem Solved**: Original screen-switching approach caused LVGL object invalidation and NULL pointer crashes
- **Solution**: Full-screen overlay on top of current screen instead of loading new screen
- **Key Insight**: `lv_obj_create(current_screen)` creates child overlay, preserving parent screen objects
- **Wake Mechanism**: Simply delete overlay → underlying screen reappears (no navigation needed)
- **Advantage**: Wakes to exact same screen you left, no object corruption, simpler code

**Power Settings Applied in Standby**:
- Display backlight: 0% (PWM OFF)
- WiFi scanning: Disabled
- AP mode: Disabled
- GPS module: Remains ON (continuous tracking requirement)
- Task update rates: Unchanged (future optimization opportunity)

**Button State Machine Enhancement**:
- Added `EXTRA_LONG_PRESS` event type (4-second threshold)
- Dual-threshold detection: checks 4s first, then 2s (prevents Settings opening during standby entry)
- Button state includes `extra_long_press_triggered` flag

**Standby Manager Module** (`src/utils/standby_manager.cpp`, `include/utils/standby_manager.h`):
- `enterStandby()` - Creates overlay, saves state, starts 3s timer
- `wakeFromStandby()` - Restores power settings, removes overlay
- `isStandby()` - Query current state
- `getStats()` - Retrieve usage statistics
- `StandbyState` enum: ACTIVE, ENTERING, STANDBY, WAKING

**Implementation Details**:
```cpp
// Create overlay on current screen (not new screen!)
lv_obj_t* current_screen = lv_scr_act();
g_standby_screen = lv_obj_create(current_screen);  // Child of current screen
lv_obj_set_size(g_standby_screen, LV_HOR_RES, LV_VER_RES);
lv_obj_set_style_bg_color(g_standby_screen, lv_color_black(), 0);
lv_obj_move_foreground(g_standby_screen);  // Bring to front

// Wake: simply delete overlay
lv_obj_del(g_standby_screen);  // Current screen reappears
```

**Critical Bug Fixed During Development**:
- **Issue**: NULL pointer crash (LoadProhibited at 0x00000020) when waking from standby
- **Root Cause**: Original `lv_scr_load()` approach invalidated radar canvas objects
- **Solution**: Overlay approach eliminates screen switching entirely
- **Result**: Zero crashes, clean wake transition

**Build Impact**: +3,592 bytes flash (1,394,653 bytes total, 44.3%), +32 bytes RAM

**Files Added**:
- `include/utils/standby_manager.h` - Public API and types
- `src/utils/standby_manager.cpp` - Implementation (280 lines)

**Files Modified**:
- `include/hardware/input/button.h:14-20` - Added EXTRA_LONG_PRESS event
- `include/hardware/input/button.h:26` - Added extra_long_press_ms config
- `include/hardware/input/button.h:37` - Added extra_long_press_triggered flag
- `src/hardware/input/button.cpp:62-84` - Dual-threshold detection logic
- `src/core/device_manager.cpp:6` - Added standby_manager include
- `src/core/device_manager.cpp:412-420` - Added EXTRA_LONG_PRESS case, standby wake check
- `src/core/main.cpp:29` - Added standby_manager include
- `src/core/main.cpp:294-297` - Added standby manager initialization
- `src/ui/navigation.cpp:62-77` - Added radar canvas validation (defensive)
- `src/ui/navigation.cpp:527-545` - Enhanced updateRadarDisplay() validation

**Serial Commands**: None (future: `standby stats`, `standby enter`, `standby wake`)

**Reference Documentation**: [`docs/standby_mode.md`](docs/standby_mode.md) - Complete technical guide (to be created)

---

## [v0.11.0] - 2025-10-21

### Changed

**Display Rotation System (Enclosure Design Adaptation)**
- Software rotation to compensate for 90° CCW physical display rotation in enclosure
- LVGL software rotation configured BEFORE driver registration (critical for RGB panels)
- User sees UI upright despite physical display orientation change
- Touch input automatically transformed to match rotated display

**User Experience**:
- **Before**: UI would appear sideways if display physically rotated in enclosure
- **After**: Software 90° CW rotation compensates, UI appears upright to user
- **Result**: Seamless adaptation to enclosure design changes without hardware modifications

**Technical Details**:
- **Rotation Method**: LVGL 8.x `disp_drv.sw_rotate = 1` + `disp_drv.rotated = LV_DISP_ROT_90`
- **Configuration**: `system_config::display::ROTATION_DEGREES = 90` constant
- **Critical Timing**: Rotation must be set BEFORE `lv_disp_drv_register()` call
- **RGB Panel Limitation**: Post-registration `lv_disp_set_rotation()` only transforms touch, not graphics
- **Touch Transform**: LVGL automatically adjusts touch coordinates for rotated display
- **Frame Buffer**: No changes required - software rotation handles pixel remapping

**Implementation Approach**:
```cpp
// CORRECT: Set rotation properties before registration
disp_drv.sw_rotate = 1;
disp_drv.rotated = LV_DISP_ROT_90;
lv_disp_t* disp = lv_disp_drv_register(&disp_drv);

// WRONG: Post-registration only rotates touch input
lv_disp_t* disp = lv_disp_drv_register(&disp_drv);
lv_disp_set_rotation(disp, LV_DISP_ROT_90);  // Graphics stay unrotated!
```

**Build Impact**: No flash/RAM impact (compile-time constant, existing LVGL feature)

**Files Modified**:
- `include/core/system_config.h:36-37` - Added `ROTATION_DEGREES = 90` constant
- `src/core/device_manager.cpp:2` - Added `#include "system_config.h"`
- `src/core/device_manager.cpp:453-477` - Implemented pre-registration software rotation
- `CLAUDE.md:223-256` - Added Display Rotation documentation section

**Known Limitations**:
- Rotation is compile-time constant (requires rebuild to change)
- Only supports 90°/180°/270° rotations (LVGL limitation, not arbitrary angles)
- RGB panels require software rotation (hardware rotation not supported)

**Future Enhancements**:
- Runtime rotation selection via settings UI (if needed for different enclosure variants)
- NVS persistence of rotation preference

**Reference Documentation**:
- LVGL 8.x Display Rotation: https://docs.lvgl.io/8.3/porting/display.html#rotation
- ESP32 RGB Panel Characteristics: Requires pre-registration rotation configuration

### Improved

**Screen Tearing Reduction (Extra-Large LVGL Buffers)**
- Increased LVGL buffer size to 160 lines (maximum practical size)
- Minimizes "staircase" tearing artifacts during scrolling and screen transitions
- Reduces visible split/offset rendering where half of elements update before the other half

**User Experience**:
- **Before**: Visible diagonal "staircase" pattern during screen transitions (radar → settings)
- **Before**: Scrolling showed split buttons/elements (half moves first, other half follows)
- **After**: Significantly reduced tearing (3 flush operations instead of 10)
- **Result**: Much smoother display updates, though not completely tear-free

**Technical Details**:
- **Root Cause**: Asynchronous DMA transfers from PSRAM while display scans (no sync mechanism)
- **Attempted Solution**: Hardware bounce buffer (not supported in this ESP-IDF version)
- **Actual Solution**: Dramatically increased LVGL buffer size from 120 to 160 lines
- **Buffer Configuration**: 160 lines × 480 pixels × 2 bytes × 2 buffers = 295KB PSRAM
- **Flush Reduction**: 480÷160 = 3 flushes per frame (vs 4 with 120 lines, 10 with 50 lines)

**Why Larger Buffers Help**:
- Each flush operation creates a visible horizontal band during scrolling
- 10 bands (50-line buffers) were very noticeable
- 4 bands (120-line buffers) were less visible
- 3 bands (160-line buffers) are even less perceptible
- Partial refresh mode only updates changed areas for performance

**Limitations**:
- Tearing cannot be completely eliminated without hardware sync (VSYNC callback or bounce buffer)
- ESP-IDF version used doesn't support `bounce_buffer_size_px` feature
- 160 lines is practical maximum (larger buffers provide diminishing returns)

**Build Impact**: +73KB PSRAM usage (295KB total vs 221KB with 120 lines)

**Files Modified**:
- `include/core/system_config.h:34` - Changed `BUFFER_LINES` from 120 to 160
- `CHANGELOG.md:68-98` - Documented tearing reduction approach and limitations

---

**Smooth Scrolling in Settings UI (Display Buffer Optimization)**
- Dramatically increased LVGL buffer size from 50 to 120 lines
- Fixed visible horizontal band/block artifacts during Settings screen scrolling
- Reduced flush operations from 10 to 4 per frame (60% reduction)

**User Experience**:
- **Before**: Visible horizontal bands/blocks during scrolling (10 separate flush operations)
- **After**: Smooth, professional scrolling without tearing artifacts (only 4 flush operations)
- **Result**: Settings UI feels polished and responsive like commercial products

**Technical Details**:
- **Root Cause**: Small 50-line buffers required 10 flush operations per full screen update
- **Solution**: Increased buffer size to 120 lines (480×120 = 57,600 pixels per buffer)
- **Flush Reduction**: 480÷120 = 4 flushes per frame (vs 480÷50 = 10 flushes previously)
- **Rendering Behavior**: Fewer, larger chunks = less visible banding during scrolling
- **Refresh Mode**: Partial refresh (`full_refresh = 0`) for optimal performance

**Why Larger Buffers Work**:
- Each flush operation creates a visible horizontal band during motion
- 10 bands are very noticeable to the human eye during scrolling
- 4 bands are much less perceptible and create smoother visual experience
- Partial refresh mode still provides fast rendering by only updating changed areas

**Build Impact**: +129KB PSRAM usage (1.6% of 8MB available)

**Files Modified**:
- `include/core/system_config.h:34` - Changed `BUFFER_LINES = 50` to `BUFFER_LINES = 120`
- `src/core/device_manager.cpp:450-451` - Updated comment to reflect buffer optimization
- `CLAUDE.md:114-146` - Updated display optimization documentation

**Performance Impact**:
- CPU usage: No change (same partial refresh mode)
- Memory usage: +129KB PSRAM (221KB total for dual buffers)
- Flush operations: 60% reduction (4 vs 10 per frame)
- User perception: Significantly improved (smooth vs blocky scrolling)

---

## [v0.10.0] - 2025-10-20

### Added

**Heading-Up Navigation Mode (Priority 1 - Critical UX)**
- Radar rotates to match walking direction - user always moves "forward" on screen
- GPS heading extracted from NMEA RMC sentence (course and speed over ground)
- Automatic coordinate rotation system for waypoints and position indicators
- North indicator (red circle with white "N") shows true north relative to heading
- Stationary mode: maintains last heading for 10 seconds, then reverts to north-up
- Settings toggle: Switch between Heading-Up and North-Up modes (Settings > Display)
- NVS persistence: Navigation mode preference saved across reboots

**User Experience**:
- **Before**: North always at top, user must mentally rotate map when turning
- **After**: Walking direction always points up, map rotates automatically
- **Result**: Intuitive navigation matching Google Maps/Waze behavior

**Technical Details**:
- **GPS Heading**: Parsed from RMC fields 7-8 (speed knots, course degrees)
- **Heading Threshold**: 0.5 knots minimum speed for reliable heading
- **Rotation Algorithm**: `rotatePoint()` applies -heading rotation to all coordinates
- **North Indicator**: Calculated position at screen edge, rotates with heading
- **Coordinate Transform**: Applied in `latLonToScreen()` after Haversine calculation
- **Performance**: O(n) rotation per waypoint, <1ms for 50 waypoints @ 240MHz

**Build Impact**: +1,848 bytes flash (rotation system, north indicator, settings toggle)

**Files Modified**:
- `include/hardware/sensors/gps_lc76g.h:12-15` - Added course, speed, hasHeading to GPSData
- `src/hardware/sensors/gps_lc76g.cpp:37-90` - Parse RMC course/speed fields
- `include/ui/ui_manager.h:125-129` - Added heading state to UIState
- `src/ui/navigation.cpp:98-118` - Implemented rotatePoint() function
- `src/ui/navigation.cpp:158-161` - Apply rotation in latLonToScreen()
- `src/ui/navigation.cpp:248-286` - Added drawNorthIndicator() function
- `src/ui/navigation.cpp:528-541` - Heading update logic with stationary fallback
- `include/settings_manager.h:37` - Added heading_up_mode setting (default: true)
- `src/ui/settings_screen.cpp:1012-1051` - Navigation mode dropdown with NVS save
- `src/ui/ui_manager.cpp:54-59` - Load heading_up_mode from NVS on startup

**Memory Usage**:
- **Flash**: 1,342,041 bytes (42.7%) - was 1,340,193 bytes
- **RAM**: 100,772 bytes (30.8%) - unchanged

---

## [v0.9.0] - 2025-10-20

### Added

**WiFi/AP Auto-Disable in CRITICAL Power Mode (Priority 2.6 Phase 3)**
- Automatic WiFi scanning disable when battery ≤ 20%
- Automatic AP mode disable when battery ≤ 20%
- Prevents accidental battery drain from forgotten WiFi/AP connections
- User can manually re-enable WiFi/AP after automatic disable (override allowed)
- Serial logging for transparency: `[POWER] ✓ WiFi scanning disabled (Critical mode - auto power save)`
- Settings persistence: WiFi/AP state saved to NVS after auto-disable

**Technical Details**:
- Implementation: `src/utils/power_manager.cpp:262-279`
- Controlled by: `applyPowerMode(PowerMode::CRITICAL)` function
- Integration: Uses `scanner::setWiFiEnabled(false)` and `wifi_manager::setEnabled(false)`
- Settings fields: `settings.wifi_enabled`, `settings.wifi_ap_enabled`
- Power savings: ~80-120mA when WiFi disabled
- User override: Manual re-enable via Settings UI works immediately

**Build Impact**: +~200 bytes flash (WiFi/AP disable logic)

**Files Modified**:
- `src/utils/power_manager.cpp:1-7` - Added scanner.h include
- `src/utils/power_manager.cpp:262-279` - Implemented WiFi/AP auto-disable in CRITICAL mode

---

## [v0.8.0] - 2025-10-19

### Added

**Waypoint Glow Effect (Priority 3.8)**
- Static soft glow around all waypoint beacons for analog radar aesthetic
- Uses LVGL native shadow rendering (no sprite assets required)
- Configurable glow parameters via RadarConfig constants
- Soft yellow-white glow (color: 0xFFFF88) with 16% opacity
- 18-pixel glow radius with 2-pixel shadow spread
- Centered glow effect (no X/Y offset) for symmetric appearance
- Professional aviation-grade radar appearance

**Technical Details**:
- Implementation: LVGL shadow properties (shadow_width, shadow_color, shadow_opa, shadow_spread)
- Configuration: New RadarConfig constants in `ui_manager.h`
  - `WAYPOINT_GLOW_RADIUS = 18` (shadow width in pixels)
  - `WAYPOINT_GLOW_COLOR = 0xFFFF88` (soft yellow-white)
  - `WAYPOINT_GLOW_OPACITY = LV_OPA_40` (16% opacity)
  - `WAYPOINT_GLOW_SPREAD = 2` (shadow spread in pixels)
- Performance: <1ms additional render time for 8-10 on-screen waypoints
- Zero heap allocation: Pure LVGL styling

**Build Impact**: +~100 bytes flash (configuration constants + styling code)

**Files Modified**:
- `include/ui/ui_manager.h:68-72` - Added glow configuration constants
- `src/ui/navigation.cpp:319-325` - Applied glow effect to waypoint drawing descriptor

---

## [v0.7.1] - 2025-10-18

### Fixed

**GPIO0 Button Polling Fix (CRITICAL)**
- **Root Cause**: `button::update()` was NEVER called anywhere in the codebase
- **Symptom**: Button worked intermittently (timing-dependent)
- **Impact**: Settings menu inaccessible, zoom controls unreliable
- **Solution**: Added button polling to UI Task (`task_manager.cpp:90-92`)
- **Result**: Button now works 100% of the time
- **Build Impact**: +14,728 bytes flash

**Technical Details**:
- Button initialization was completing successfully
- Hardware interrupt fired but state machine never processed it
- Without polling loop, button was effectively non-functional
- Serial monitor presence changed timing (USB CDC affects interrupt latency)
- Fix: Poll button every 5ms in UI Task (highest priority, Core 1)

**Files Modified**:
- `src/utils/task_manager.cpp` - Added `device_manager::updateButton()` call

### Added

**Crash Logging System (ESP32 Core Dump)**
- ESP32 panic handler with 256KB flash partition
- Three new serial commands:
  - `crash dump` - View last crash information (PC address, crashed task, core dump version)
  - `crash info` - Show system capabilities and usage instructions
  - `crash clear` - Clear crash data (note: auto-overwrites on next panic)
- Automatic panic capture to flash (survives reboots)
- Program Counter (PC) tracking for crash location identification
- Crashed task identification (UI/I2C/Network/System)
- Core dump version and firmware SHA256 tracking
- Comprehensive troubleshooting workflow documentation

**System Capabilities**:
- ✅ Automatic panic capture to flash partition
- ✅ Survives reboot (persistent storage)
- ✅ Program counter (crash location)
- ✅ Crashed task identification
- ✅ Accessible via serial commands
- ⚠️ Limitations: PC requires firmware.elf for symbol lookup, single crash storage

**Configuration**:
- Enabled `CORE_DEBUG_LEVEL=3` in `platformio.ini`
- Uses existing 256KB coredump partition from `partitions.csv`

**Documentation**:
- Added "Crash Investigation Workflow" to `docs/troubleshooting.md`
- Pattern recognition guide (single crash vs reproducible bug)
- Common crash patterns (GPIO, battery, WiFi-related)
- Preventive monitoring checklist
- Advanced debugging with addr2line tool

**Build Impact**: +7,816 bytes flash

**Files Modified**:
- `platformio.ini` - Enabled core dump debug level
- `src/utils/diagnostics.cpp` - Added crash dump commands (~110 lines)
- `docs/troubleshooting.md` - Added crash investigation guide (~150 lines)

**WiFi/AP Mode Mutual Exclusion**
- Implemented user-requested behavior: WiFi and AP modes are now mutually exclusive
- Enabling WiFi → Automatically disables AP mode (stops GPX server, disconnects AP)
- Enabling AP → Automatically disables WiFi (disconnects from network)
- Both can be OFF for battery savings
- Clear serial logging with ⚠️ warnings for mode changes
- All state changes saved to NVS (persistent across reboots)
- UI toggles update automatically without user intervention

**User Experience**:
- Before: Both WiFi and AP could run simultaneously (confusing)
- After: Only one mode active at a time (clear, predictable)
- User controls: Enable desired mode, system handles coordination
- Serial feedback: Clear warnings when automatic changes occur

**Build Impact**: +1,100 bytes flash

**Files Modified**:
- `src/ui/settings_screen.cpp` - WiFi/AP coordination logic (~50 lines)

**Total Build Impact for v0.7.1**: +23,644 bytes flash (+1.8%), RAM unchanged

---

## [v0.7.0] - 2025-10-18

### Added

**GPX Waypoint Enhancements (Priority 2.8 - Quick Wins Phase 1)**
- Refresh Waypoints button in GPS settings tab
  - Location: After Factory Reset button
  - Action: Calls `gpx_loader::refreshGPXFiles()` to reload from SD
  - Button: "🔄 Refresh Waypoints" (blue, 200x40px)
  - Feedback: Serial log "Loaded X waypoints from SD card"
  - Updates radar display and waypoint count label
  - Implementation: `src/ui/settings_screen.cpp:1612-1632`

- Waypoint Count Indicator in GPS settings tab
  - Display: "Waypoints: 15/50" (current/max)
  - Color coding:
    - Green (0x00FF00): 0-30 waypoints
    - Yellow (0xFFFF00): 31-45 waypoints
    - Red (0xFF4444): 46-50 waypoints (approaching limit)
  - Dynamic updates via `updateWaypointCountLabel()` function
  - Implementation: `src/ui/settings_screen.cpp:1585-1610`

- Build impact: +752 bytes flash, no RAM change

### Improved

**Settings UI/UX Improvements (Priority 2.13)**
- Added 100px bottom padding to all settings tabs
  - GPS tab (line 628)
  - WiFi tab (line 774)
  - Display tab (line 844)
- Benefits:
  - Bottom elements can scroll to middle of screen
  - Improved touch accuracy for bottom buttons
  - Better readability of bottom text elements
  - Professional app-like scrolling experience
- Build impact: +16 bytes flash

**Waypoint Filtering System Documentation**
- Complete technical documentation in `docs/waypoint_filtering.md`
- Dual-strategy filtering system:
  - Distance-based filtering (10× zoom radius multiplier)
  - Sector-based clustering (maximum 8 off-screen indicators)
- Performance characteristics documented
- Algorithm references added to CLAUDE.md

---

## [v0.6.0] - 2025-10-17

### Added

**Loading Screen with LVGL Spinners (Priority 2.7)**
- Professional boot sequence with animated spinner
- Implementation details:
  - 75px spinner with ease-in-out animation
  - 2-second rotation period for smooth motion
  - Title: "GPS RADAR SYSTEM" (Iosevka 20pt)
  - Status: "Initializing..." (Iosevka 16pt)
  - Dark grey background (#262626)
  - 5-second display duration
  - Automatic transition to radar screen
- Benefits:
  - Professional startup experience
  - Clear system initialization indicator
  - Reduces user confusion during GPS lock
  - Smooth transition animations

**Key Files**:
- `src/ui/ui_manager.cpp` - Loading screen creation
- `src/core/main.cpp` - Boot sequence integration
- `include/core/system_config.h` - Configuration constants

**Actual Time**: 2 hours (as estimated)

---

## [v0.5.0] - 2025-10-15

### Added

**Battery Percentage Display on Radar Screen (Priority 1.6)**
- Always-visible battery percentage (top-right corner)
- Color-coded status indicators:
  - Green (0x00FF00): >70% battery
  - Yellow (0xFFFF00): 50-70% battery
  - Red (0xFF0000): <50% battery
- Auto-updates every 5 seconds via System Task
- Simple percentage-only format: "69%"
- Integrated with existing battery monitoring system

**Critical Implementation Details**:
- Position: `-150px from right, +20px from top`
  - Circular clipping requires aggressive inset
  - Initial `-50px` caused text cutoff and system crashes
- Short text format prevents cutoff on circular boundary
- Z-order: Created before shadow overlay for visibility
- Updates integrated into System Task (`task_manager.cpp`)

**Display Location**:
```
┌─────────────────────────────────┐
│                            69%  │ ← Top-right (-150px, +20px)
│                                 │
│         RADAR DISPLAY           │
│                                 │
│                                 │
│    GPS: Fixed (6 sats)          │ ← Bottom-center
└─────────────────────────────────┘
```

**Battery Monitoring vs Display**:
Two separate but connected systems:
1. **Monitoring System** (`battery.cpp`)
   - Collects voltage samples via GPIO4 ADC
   - Performs trend analysis (charging/discharging/stable)
   - Provides serial diagnostics
   - Controlled via `battery monitor on|off` command

2. **Display System** (`ui.battery_label`)
   - Visual percentage indicator on radar
   - Always visible and updating
   - Independent of serial monitoring setting

**Serial Commands**:
```
battery status         # Show voltage, percentage, state
battery monitor on     # Enable periodic serial logging (every 60s)
battery monitor off    # Disable periodic logging (UI still works)
battery voltage        # Show current voltage reading
battery history        # Show voltage trend history
```

**Key Files**:
- `include/ui/ui_manager.h:99` - Battery label field
- `src/ui/ui_manager.cpp:129-135` - Label creation with safe positioning
- `src/utils/task_manager.cpp:695-714` - Update logic with color coding
- `src/hardware/sensors/battery.cpp` - Battery monitoring system

**What We Skipped** (as requested):
- Charging icon/animation on display (use board CHG LED instead)
- Voltage display on screen (available via serial only)
- Complex battery state transitions on UI (serial only)

**Reference Documentation**:
- `docs/battery_monitoring.md` - Complete battery system guide
- `docs/battery_display_summary.md` - Implementation history

---

## [v0.4.0] - 2025-10-12

### Added

**GPS Settings UX Simplification (Priority 2.5)**

**Background**:
User testing identified two critical UX issues:
1. GNSS Systems Checkboxes (5 checkboxes) overwhelming for average users
2. Update Rate Manual Selection confusing and error-prone

**Solution: Smart Presets System**

**GNSS Systems → Preset Dropdown**:
- Replaced 5 checkboxes with intelligent preset system
- 5 options: Battery Saver, Balanced (default), Best Accuracy, Maximum, Custom...
- "Custom..." option opens modal with 5 checkboxes for advanced users
- Default: "Balanced" (GPS + GLONASS, ~55 satellites, global coverage)

**GNSS Preset Mappings**:

| Preset | Systems Enabled | Bitmask | Satellites | Use Case |
|--------|----------------|---------|------------|----------|
| Battery Saver | GPS only | `0x01` | ~31 | Longest battery life, acceptable accuracy |
| Balanced (Default) | GPS + GLONASS | `0x03` | ~55 | Best balance of accuracy/battery |
| Best Accuracy | GPS + GLONASS + Galileo | `0x07` | ~85 | High accuracy for navigation |
| Maximum | GPS + GLONASS + Galileo + BeiDou | `0x0F` | ~120 | Maximum accuracy, faster fix |
| Custom | User-defined | Variable | Variable | Advanced users only |

**Update Rate → Auto-Calculated**:
- Removed manual dropdown entirely
- Auto-calculate rate based on positioning mode:
  - Pedestrian → 1 Hz (1000 ms) - walking speed
  - Automotive → 5 Hz (200 ms) - driving speeds
  - Fitness → 2 Hz (500 ms) - running/cycling
  - Aviation → 10 Hz (100 ms) - high-speed flight
- Display: "Update Rate: 5 Hz (auto)" (read-only indicator)

**Implementation Complete**:
- ✅ Backend helper functions with comprehensive logging
- ✅ Help modal system (4 help icons)
- ✅ Phase 1-7 all completed and tested
- ✅ Comprehensive documentation (`docs/gps_settings_simplification.md`)

**Serial Logging**:
All GPS settings changes logged with consistent tags:
```
[GNSS_PRESET]   # GNSS preset selection and bitmask mapping
[GNSS_CONFIG]   # GNSS configuration breakdown
[AUTO_RATE]     # Auto update rate calculation
[GPS_SETTINGS]  # General GPS settings changes
[SETTINGS]      # NVS save/load operations
```

**Success Criteria Achieved**:
- ✅ Average users can configure GPS without technical knowledge
- ✅ Preset names are self-explanatory
- ✅ Update rate is automatically optimized
- ✅ Advanced users can still access full control via "Custom..."
- ✅ All settings persist across reboots

**External Zoom Button (Priority 1.1)**
- GPIO0 button used for zoom cycling
- Hardware button successfully implemented
- Touch screen freed for waypoint interaction
- Maintains 5-level zoom cycle behavior

**Settings Menu Trigger (Priority 1.2)**
- GPIO0 long-press detection (2-3 seconds)
- Enter/exit settings menu working
- Non-blocking button detection implemented
- Settings accessible via long-press

**NVS Storage System (Priority 2.1)**
- Non-Volatile Storage (NVS) for persistent settings
- User preferences stored across reboots
- Safe write operations with error handling
- Default values on first boot implemented

**Settings Menu UI (Priority 2.2)**
- Full-screen settings interface with tabbed navigation
- Touch-based navigation working
- Visual feedback for all controls
- Settings automatically saved (no explicit Save button)
- Red X button returns to radar screen

**Settings Categories**:
1. Display Settings (zoom, grid, brightness)
2. GPS Settings (update interval, logging, GNSS systems, positioning mode)
3. Waypoint Settings (persistent storage, max waypoints)
4. Advanced Settings (show coordinates, heading, speed)

---

## [v0.3.0] - 2025-10-12

### Added

**WiFi Web Portal for GPX Upload (Priority 3.1)**

**Architecture**: Station Mode + Web Portal
- User connects radar to home/office WiFi
- Web portal accessible at `http://radar.local` or device IP address
- No mode switching needed - works on existing WiFi network

**Current Implementation**:
- ✅ WiFi Manager - Full STA mode with credential storage
- ✅ GPX Server - Complete web server with upload UI (`gpx_server.cpp`)
- ✅ Beautiful drag-and-drop interface (HTML/CSS/JS embedded)
- ✅ RESTful API - Upload, list, delete endpoints
- ✅ mDNS support - `http://radar.local` automatic discovery
- ✅ Integration - GPX server integrated in main.cpp loop (auto-starts when WiFi connects)
- ✅ Auto-loading - GPX files auto-loaded from SD card on boot (`gpx_loader.cpp`)
- ✅ Web Portal UI - URL displayed in WiFi settings tab

**Known Issues**:
- ⚠️ Web portal only accessible in AP mode
- ⚠️ mDNS not working in STA mode
- ✅ Workaround: Use IP address displayed in WiFi settings tab

**Web Portal Features**:
- 📂 Drag-and-drop GPX file upload
- 📋 View uploaded GPX files with delete option
- 🎨 Beautiful responsive UI (purple gradient design)
- 🔄 Real-time upload progress and status messages
- 🌐 mDNS discovery - `http://radar.local`
- 📍 Auto-creates `/gpx/` folder on SD card

**Web Portal Endpoints**:
```
GET  /              # Upload interface (HTML page)
POST /upload        # File upload handler
GET  /list          # List GPX files (JSON)
DELETE /delete/:filename  # Delete GPX file
```

**User Workflow**:
1. Long-press GPIO0 → Settings screen
2. Connect to WiFi network (via WiFi UI)
3. Settings displays: "Web Portal: http://192.168.1.100"
4. Open browser on phone/laptop → Drag-and-drop GPX files
5. Waypoints automatically appear on radar

---

## [v0.2.0] - 2025-10-07

### Added

**Multi-Level Zoom System (Priority 1.3)**
- 5 zoom levels with progressive grid sizing
- Touch-to-zoom interface (tap canvas to cycle zoom)
- Dynamic meters-per-pixel calculation
- Grid spacing adapts to zoom level (48px → 140px squares)
- Zoom level displayed in GPS status text

**Zoom Levels**:

| Level | Radius | Grid Spacing | Grid Size (pixels) | Use Case |
|-------|--------|--------------|-------------------|----------|
| 10km | 10000m | 2000m | ~48px | Long-range navigation |
| 1km | 1000m | 300m | ~72px | Local area |
| 500m | 500m | 200m | ~96px | Neighborhood |
| 100m (default) | 100m | 50m | ~120px | Street level |
| 10m | 10m | 5.83m | ~140px | Precision mode |

**User Interaction**:
- Tap radar canvas to cycle zoom: 10km → 1km → 500m → 100m → 10m → (loop)
- Current zoom level shown in status text: `GPS: Fixed (15 sats) [100m]`

**Key Files**:
- `include/ui/ui_manager.h` - ZoomLevel enum, ZoomConfig struct
- `src/ui/ui_manager.cpp` - Touch-to-zoom event handler
- `src/ui/navigation.cpp` - Zoom-aware coordinate conversion

**Edge-Aligned Grid System (Priority 1.4)**
- Perfect edge alignment at all zoom levels
- Grid lines always reach screen edges (0 and 479 pixels)
- No visual gaps or offsets
- Dynamic grid spacing based on zoom level

**Algorithm**:
```cpp
// Draw vertical lines from x=0 with grid_spacing_pixels intervals
for (int x = 0; x < screen_size; x += grid_spacing_pixels) {
    draw_line(x, 0, x, screen_size-1);
}
// Always draw right edge line at x=479
if ((screen_size - 1) % grid_spacing_pixels != 0) {
    draw_line(screen_size-1, 0, screen_size-1, screen_size-1);
}
```

**Key Files**:
- `src/ui/navigation.cpp:drawRadarGrid()` - Grid drawing implementation

**Visual Polish and UI Refinements (Priority 1.5)**
- ✅ Fixed triangle color (black → red #D43701)
- ✅ Geometric centering of center triangle (using centroid offset)
- ✅ Changed waypoint markers to circles (25x25px yellow)
- ✅ Removed "+ Waypoint" button (clean full-screen radar)
- ✅ Repositioned GPS status text higher (y=-40 instead of y=-10)
- ✅ Reduced GPS serial logging spam (every 10 seconds instead of every second)
- ✅ Removed "[RADAR] Update display" debug spam
- ✅ Fixed canvas positioning (explicit 0,0 with no padding)

**GPS Status Label**:
- Position: `ALIGN_BOTTOM_MID, y_offset=-40`
- Font: `lv_font_montserrat_14`
- Colors:
  - Searching: Yellow (#FFFF00)
  - Fixed: Green (#00FF00)
- Format: `"GPS: Fixed (15 sats) [100m]"`

**GPS Integration (Priority 1.2)**
- LC76G GPS module integration (UART on GPIO43/44, 115200 baud)
- NMEA sentence parsing (GGA, RMC)
- Real-time position accuracy: ±1-2 meters (15 satellites, HDOP=1.0)
- Visual GPS status indicator:
  - Yellow text: "GPS: Searching..." (no fix)
  - Green text: "GPS: Fixed (X sats)" (locked)
- Automatic center reference update (user position becomes center)

**Performance**:
- Position update rate: 1Hz (every second)
- Serial logging: Every 10 seconds (reduced spam)
- Typical accuracy: ±0.78m latitude, ±0.46m longitude

**GPS Data Structure**:
```cpp
struct GPSData {
    double lat, lon;      // Decimal degrees
    float alt;            // Altitude in meters
    float hdop;           // Horizontal dilution of precision
    int sats;             // Number of satellites
    bool valid;           // Fix status
};
```

**Key Files**:
- `src/gps/gps_lc76g.cpp` - GPS driver and NMEA parsing
- `src/utils/task_manager.cpp` - GPS task and serial logging
- `include/device_manager.h` - GPS data structures

---

## [v0.1.0] - 2025-10-07

### Initial Release

**Core Radar Display System (Priority 1.0)**
- Full-screen circular 480x480 radar display (green background)
- Edge-aligned black grid system (2px lines)
- Red equilateral triangle in center (44x44px, geometrically centered)
- Yellow circular waypoint beacons (25x25px)
- User-centered navigation (center triangle represents user position)
- Waypoints move relative to user as GPS position updates

**Key Files**:
- `src/ui/ui_manager.cpp` - Radar screen creation and canvas setup
- `src/ui/navigation.cpp` - Drawing functions and coordinate conversion
- `include/ui/ui_manager.h` - Radar configuration constants

**Technical Implementation**:
```cpp
// Radar visual elements
CENTER_TRIANGLE_SIDE = 44px     // Equilateral triangle sides
CENTER_TRIANGLE_HEIGHT = 38px   // Triangle height
WAYPOINT_SIZE = 25px            // Yellow beacon circles
GRID_LINE_WIDTH = 2px           // Black grid lines
```

**Radar Display Features**:
- 480×480 pixel circular display
- Green background (#00AA00)
- Black grid overlay (2px lines)
- Red center triangle (user position)
- Yellow waypoint markers
- Real-time GPS coordinate tracking
- Serial debug output (115200 baud)

---

## Development Notes

### Documentation Updates
- Complete battery monitoring guide in `docs/battery_monitoring.md`
- Battery display implementation summary in `docs/battery_display_summary.md`
- GPS settings simplification guide in `docs/gps_settings_simplification.md`
- Waypoint filtering technical deep-dive in `docs/waypoint_filtering.md`
- Custom fonts documentation in `docs/custom_fonts.md`
- WiFi implementation guide in `docs/wifi_implementation_guide.md`

### Build System
- PlatformIO project with ESP32-S3 support
- 16MB Flash, 8MB PSRAM
- Custom partition table for app and filesystem
- LVGL 8.3.11 graphics library
- Arduino framework

### Hardware Requirements
- Waveshare ESP32-S3-Touch-LCD-2.1 board
- Beitian BH-880 GPS + Compass module (GPIO 43/44 UART, GPIO 15/7 I2C)
- 3.7V LiPo battery
- MT3608 boost converter (3.3V → 5V for GPS module)

---

**Project Repository**: https://github.com/alvroga/db-radar
**License**: CC BY-NC-SA 4.0 — Alvaro Robles

**Last Updated**: 2026-05-09
