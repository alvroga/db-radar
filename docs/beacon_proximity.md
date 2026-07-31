# Beacon Proximity System

**Status**: Complete ✅
**Implemented**: 2026-01 (see CHANGELOG.md [Unreleased])
**Hardware**: ESP32-S3 BLE radio, QMI8658 buzzer via EXIO TCA9554

## Overview

The Beacon Proximity system allows the radar to act as a BLE-based item finder. When zoomed to 50m, the device scans for a configured target Bluetooth beacon MAC address and provides two simultaneous feedback channels:

1. **Visual**: A cyan arc gauge drawn around the radar circle fills clockwise as signal strength increases (stronger = closer).
2. **Audio**: A sonar-style beeper increases pulse rate as the user approaches the beacon.

This turns the radar into a precision finding tool for lost items tagged with BLE beacons (e.g., AirTags, Tile, custom BLE peripherals).

## Feature Activation

Beacon scanning is **zoom-gated** — it only activates at 50m zoom level and stops automatically when the user zooms out. This prevents unnecessary BLE radio activity during normal navigation and saves power.

```
Zoom ≥ 100m → Beacon scanning OFF (idle)
Zoom = 50m  → Beacon scanning ON (active BLE scan every 2 seconds)
```

On standby enter, zoom resets to 100m which stops beacon scanning automatically.

## Visual: Arc Gauge

### Design

- **Shape**: Circular arc drawn at the outer edge of the radar display
- **Color**: Cyan (`#5DD8D8`) — consistent across light and dark modes
- **Start position**: 12 o'clock (top, LVGL 270°)
- **Fill direction**: Clockwise
- **Line width**: 14 pixels
- **Radius**: 228 pixels (near the display edge)
- **Maximum arc**: 355° (capped to prevent LVGL rendering artifacts at 360°)
- **Minimum visible arc**: 10° (any detection shows at least a sliver)

### RSSI-to-Ring Mapping

> ⚠️ The bullet list above describes a **partial arc that filled by angle**. That is not what the
> code does and has not been for some time — the indicator is a full 360° ring that grows *inward*
> as the signal strengthens, and at CLOSE it becomes a solid fill with the orange ball and star.
> Left in place as a record of the original design; the description below is the current one.

**Ring width is continuous in `rssi_display`** (updated 2026-07-31, backlog §7.3c). It used to be four
discrete widths selected by `state.zone`, which meant the ring sat perfectly still through most of an
approach and then jumped — while `rssi_display`, the slow EMA computed specifically to drive it, was
read by nothing at all.

| `rssi_display` | Ring width |
|---|---|
| ≤ -90 dBm | 6 px |
| -77.5 dBm | 20 px |
| ≥ -65 dBm | 34 px |

`width = 6 + clamp((rssi_display + 90) / 25, 0, 1) × 28`, outer edge pinned at 239 px so the ring
always sits flush with the physical circular bezel regardless of thickness.

The two decisions *around* the ring stay discrete, and therefore keep their hysteresis — this is the
project's general rule: **continuous for the analogue quantity, hysteresis only where a genuinely
discrete decision is made.**

- **Draw a ring at all?** No, if `zone == OUT_OF_RANGE`.
- **Switch to the solid CLOSE fill?** Yes, once `zone == CLOSE`.

### Code Reference

- Drawing: `src/ui/navigation.cpp` — `drawBeaconProximityGauge()`
- Called from: `src/ui/navigation.cpp` — `radarDrawEventCb()` (last draw layer)

## Audio: Sonar Beeping

### Sonar tempo — continuous in RSSI

**Rewritten 2026-07-31.** The tempo was four discrete rates selected by `state.zone`
(1500/750/500/250 ms). Field report: *"the rate at which the beeping changes is very difficult to
gauge where to go."* With four steps, most of a search happens **inside** one zone, where moving
produces no audible change at all — and that is fatal here specifically, because someone hunting a
beacon is not judging absolute loudness, they are listening for *change in response to their own
movement*. A step function gives them none until they happen to cross a boundary.

Tempo is now continuous and **linear in dBm**:

```
t        = clamp((rssi_ema + 90) / 40, 0, 1)
interval = 1500 × (150/1500) ^ t     ms
```

| `rssi_ema` | Interval |
|---|---|
| ≤ -90 dBm | 1500 ms |
| -80 dBm | ~843 ms |
| -70 dBm | ~474 ms |
| -65 dBm | ~356 ms |
| -60 dBm | ~267 ms |
| ≥ -50 dBm | 150 ms |

Linear in dBm is the correct curve, not an approximation: RSSI ≈ C − 20·log₁₀(d), so equal steps in
dBm are equal *ratios* of distance. That is the same geometric mapping the waypoint sonar uses over
metres, reached from the other direction.

### Timbre — beep length encodes the trend

"Warmer / colder" is far more actionable than absolute level when hunting, because absolute RSSI
depends on the environment, the tag's orientation and your own body — none of which the user knows —
whereas the *sign of the change* in response to a step is meaningful regardless. The trend had been
computed since the v2 redesign and read by **nothing**.

The buzzer is a bare on/off line through the IO expander, so there is no pitch to modulate. But beep
duration is already a parameter of `setSonarInterval()`, so this costs nothing:

| Trend | Beep duration | Sounds like |
|---|---|---|
| APPROACHING | 60 ms | a fuller tone — *warmer* |
| STABLE / UNKNOWN | 30 ms | neutral |
| DEPARTING | 12 ms | a clipped tick — *colder* |

Beeping can be independently enabled/disabled in Settings (Settings > Sound > Beacon Sound toggle)
without disabling the visual ring.

### Priority over the waypoint sonar

**The beacon always wins.** A beacon is a thing you are trying to *find*; a fixed waypoint is an area
you are walking into, and its sonar is a secondary convenience. So when `beacon_proximity::isInRange()`
becomes true, `updateWaypointFixSonar()` **releases the waypoint fix outright** rather than merely
yielding the buzzer — leaving it fixed would keep every other waypoint hidden from the radar and
would re-take the sonar the moment the beacon dipped out of range.

This is safe against flicker because `isInRange()` reads the *confirmed* zone, which requires 1000 ms
of hysteresis-gated agreement to enter, and OUT_OF_RANGE is below -90 dBm.

> **Historic bug, fixed 2026-07-31**: `updateWaypointFixSonar()` used to call `suppressSonar(true)`
> unconditionally as soon as a waypoint was fixed at 50 m zoom, and *then* compute the tempo. With the
> waypoint beyond 50 m the tempo was 0 → `stopSonar()`. So a fixed-but-distant waypoint permanently
> muted the beacon while making no sound itself — the beacon appeared completely dead.

### Code Reference

- Sonar update: `src/hardware/connectivity/beacon_proximity.cpp` — `updateSonar()`
- Priority / auto-unfix: `src/ui/navigation.cpp` — `updateWaypointFixSonar()`
- Buzzer API: `include/hardware/buzzer.h` — `setSonarInterval()`, `stopSonar()`
- Buzzer state machine: `src/hardware/buzzer.cpp`

## RSSI Processing

Raw BLE RSSI readings are noisy. The system applies two layers of smoothing and stability logic:

### EMA Smoothing — time constants, not per-sample α

**Updated 2026-07-31 (backlog §7.3b).** These were fixed α values tuned against a feed that was stuck
at 2 Hz. Raising the sample rate without touching them would have silently made both EMAs 2.5–5×
faster and noisier — the exact class of defect (*a rate constant nobody re-derived after the pipeline
around it changed*) that the whole subsystem audit was about.

Each accepted advertisement is blended with an α derived from the **measured** gap since the last one:

```
α    = 1 − e^(−dt / τ)
ema += α × (rssi_raw − ema)
```

`dt` is measured rather than assumed because BLE advertising is lossy — advertisements do not arrive
on a schedule, and a dropped one must widen that sample's weight rather than skew the time constant.

| EMA | τ | Drives |
|---|---|---|
| `rssi_ema` | 0.5 s | zone, trend, labels |
| `rssi_display` | 1.0 s | ring width (second stage, fed from `rssi_ema`) |

τ = 0.5 s replaces α = 0.4 at 2 Hz — which was effectively a ~2.5 s time constant — so the response is
about 5× faster *and* smoother, because it now averages more samples over that time. Nothing uses raw
RSSI.

### Zone Detection with Hysteresis

Zones prevent rapid oscillation at zone boundaries:

| Zone | RSSI Threshold | Role |
|------|---------------|------|
| OUT_OF_RANGE | < -90 dBm | Silent, no ring |
| VERY_FAR | -90 to -85 dBm | — |
| FAR | -85 to -75 dBm | — |
| MEDIUM | -75 to -65 dBm | — |
| CLOSE | ≥ -65 dBm | Solid fill + ball + star |

> The zone no longer selects the beep rate — see *Sonar tempo* below. It decides only *whether* to
> beep at all and *whether* to switch to the solid CLOSE visual, both of which are genuinely discrete
> decisions and therefore keep their hysteresis.

- **Hysteresis**: ±3 dBm band — must exceed threshold by 3 dBm to change zones
- **Confirmation**: the new zone must hold for **1000 ms**. This is a *duration*, not a sample count.
  It was "2 consecutive readings", which meant 1.0 s only because samples arrived at 2 Hz; at 5 Hz that
  would have quietly become 0.4 s and at 10 Hz 0.2 s — i.e. no confirmation at all.

### Movement Trend Detection

Diagnostic only — nothing in the system acts on the trend. Least-squares regression of `rssi_ema`
against **real time** over a 4 s window, in dBm/second:

- `slope > +1 dBm/s` → APPROACHING
- `slope < -1 dBm/s` → DEPARTING
- `|slope| ≤ 1 dBm/s` → STABLE

The threshold comes from the physics rather than from taste: walking at 1.4 m/s toward a beacon with
path-loss exponent n = 2 gives `dRSSI/dt = 8.686·v/d` ≈ 0.6 dBm/s at 20 m, 1.2 at 10 m, 2.4 at 5 m. So
1 dBm/s marks "genuinely closing" from roughly 15 m in. (The previous ±2 dBm per *cycle* at 2 Hz was
4 dBm/s, which essentially never fired outside ~3 m — and, being per-cycle, would have meant something
different again at any other rate.)

Available via serial: `beacon trend`

## ⚠️ The tag MUST advertise in Legacy (BLE 4.0) mode

`CONFIG_BT_NIMBLE_EXT_ADV` is **not set** in `sdkconfig.cc-radar`, so this firmware compiles the
`ble_gap_disc()` path and can only receive **legacy** advertisements. If the tag's configurator app is
switched to *Extend Advertisement (BLE 5.0)* or *PHY Coded (BLE 5.0)*, the radar will not see it
**at all** — and the symptom is indistinguishable from a dead tag: RSSI pinned at -127 dBm,
OUT_OF_RANGE, silent, with everything else in the system reporting perfectly healthy.

Diagnose it with `beacon status`: **`Scan callbacks: N total, 0 matched target`** with a large N means
the scan is working fine and the tag is simply invisible to it — check the tag's Adv Mode before
suspecting anything in this firmware.

Enabling BLE 5.0 extended advertising would mean turning on `CONFIG_BT_NIMBLE_EXT_ADV`, which changes
the scan to `ble_gap_ext_disc()` and costs SRAM. Not worth it: legacy advertising is what a proximity
beacon wants anyway, and this project's SRAM budget is tight (see `memory/sram_budget.md`).

## BLE Scanning — one continuous passive scan

**Rewritten 2026-07-31 (backlog §7.3a).** The feed was capped at a hard **2.0 Hz** against a tag
advertising at 5 Hz, so ~60% of every advertisement was discarded. Three independent things caused it,
and all three are gone:

1. **Controller/host duplicate filtering.** NimBLE reports a given advertiser to `onResult` *once per
   scan* while `filter_duplicates` is set (`NimBLEScan.cpp`:
   `if (filter_duplicates && m_callbackSent) return 0;`).
2. **`g_pScan->stop()` on the first hit**, which ended the scan the instant the beacon was seen —
   guaranteeing exactly one sample per cycle.
3. **A 500 ms scan/idle poll loop** in `update()`.

Current configuration:

- **Scan mode**: **Passive**. Not only a power choice — with active scanning and a legacy `ADV_IND`
  advertiser, NimBLE *withholds* `onResult` until the scan response arrives, and failing that until
  the scan completes. Passive short-circuits that test and delivers on the advertisement itself.
- **Duration**: continuous. `start(0, …)` → `BLE_HS_FOREVER`, started once at `setEnabled(true)`,
  stopped at `setEnabled(false)`. `update()` restarts it if it ever stops on its own.
- **Window / interval**: 100 ms / 100 ms — **100% duty**.
- **Duplicate filter**: off.
- **Results storage**: `setMaxResults(0)` — callback only. Each device is freed right after its
  callback, so the results vector cannot grow during a forever-scan.
- **Filter**: target address, compared as a parsed `NimBLEAddress`. With duplicates off, `onResult`
  fires for every advertisement from *every* device in range, so the old
  `getAddress().toString()` + lowercase `String` comparison — two heap allocations per callback — was
  no longer viable.
- **Timeout**: beacon marked lost after **5 seconds** (was 15 s; at 5–10 Hz that is 25–50 missed
  advertisements, which is unambiguous).

**Expected**: 2 Hz → ~5 Hz, or ~10 Hz if the tag's advertising interval is reconfigured to 100 ms.
Verify with `beacon status`, which now reports the **measured** mean inter-arrival in both ms and Hz.

> ⚠️ **Two footguns, if you touch this code.**
>
> 1. `setAdvertisedDeviceCallbacks(cb, wantDuplicates)` calls `setDuplicateFilter(!wantDuplicates)`
>    internally. Calling it *after* `setDuplicateFilter(false)` silently re-enables filtering and puts
>    the feed straight back to 2 Hz. `debugScanAll()` did exactly that on its restore path, so
>    `beacon scan` would have quietly halved the sample rate for the rest of the session.
> 2. `debugScanAll()` runs an *active, results-storing, one-shot* scan — the opposite configuration.
>    It must stop the continuous scan, and restore **every** parameter plus the scan itself afterwards.
>
> **Power**: 100% scan duty is the one genuinely new cost. It is zoom-gated to 50 m so it is bounded,
> but if battery drain is objectionable, `SCAN_WINDOW_MS` is the single knob — 80 ms gives 80% duty and
> should still catch a 200 ms advertiser most of the time.

## Distance Estimation

A path-loss-based distance estimate is computed for display purposes:

```
distance_m = 10 ^ ((measured_power - rssi) / (10 × n))
```

- `measured_power`: Calibrated RSSI at 1 meter (default -59 dBm)
- `n`: Path loss exponent (default 2.5 — typical indoor environment; 2.0 = open air, 4.0 = dense indoor)

**Note**: This distance is a rough estimate only. RSSI-based ranging has ±50% error in real environments. The arc gauge uses raw RSSI zones, not this estimate, as proximity zones are more reliable than calculated distances.

## Settings & Persistence

All settings are stored in NVS and persist across reboots:

| Setting | NVS Key | Default | Description |
|---------|---------|---------|-------------|
| Enabled | `bcn_en` | `true` | Feature on/off |
| Sound | `bcn_snd` | `true` | Sonar beeping on/off |
| MAC | `bcn_mac` | `AA:BB:CC:DD:EE:FF` | Target beacon MAC address |
| Measured Power | `bcn_pwr` | `-59` dBm | Calibrated RSSI at 1 meter |
| Path Loss N | `bcn_n` | `2.5` | Path loss exponent |

**Settings UI**: Settings > Sound > "Beacon Sound" toggle (enable/disable audio feedback)
**Settings code**: `src/ui/settings_screen.cpp:1241-1310`
**Persistence code**: `src/utils/settings_manager.cpp:656-705`

## API Reference

```cpp
namespace beacon_proximity {
    void init();                        // Initialize BLE subsystem
    void setEnabled(bool enabled);      // Start/stop scanning
    bool isEnabled();                   // Check if currently active
    void update();                      // Run BLE scan cycle (Network Task, ~200ms)
    void updateSonar();                 // Update beep rhythm (Network Task, ~200ms)
    BeaconState getState();             // Full state snapshot
    ProximityZone getCurrentZone();     // OUT_OF_RANGE / FAR / MEDIUM / CLOSE
    MovementTrend getCurrentTrend();    // UNKNOWN / STABLE / APPROACHING / DEPARTING
    float getDistance();                // Estimated distance in meters (rough)
    bool isBeaconNearby(float threshold_m);  // Simple proximity check
    void debugScanAll();                // Print all visible BLE devices
    void debugPrintState();             // Print internal module state
    void resetState();                  // Reset EMA and trend history
}
```

## Serial Commands

```
beacon [status]              - Show beacon proximity status and current readings
beacon enable on|off         - Enable/disable the entire feature
beacon mac XX:XX:XX:XX:XX:XX - Set target beacon MAC address
beacon power -XX             - Set measured power in dBm (calibrate at 1 meter)
beacon n X.X                 - Set path loss exponent (2.0–4.0)
beacon test                  - Force a scan and report detected signal
beacon scan                  - List ALL visible BLE devices with RSSI
beacon debug                 - Print full internal module state
beacon zone                  - Show current zone, pending zone, hysteresis state
beacon trend                 - Show trend history and calculated slope
beacon reset                 - Reset all smoothing and trend state
```

## Task Integration

| Task | Operation | Interval |
|------|-----------|---------|
| Network Task (Core 0) | `beacon_proximity::update()` | ~200ms |
| Network Task (Core 0) | `beacon_proximity::updateSonar()` | ~200ms |
| Network Task (Core 0) | Zoom gating check | On zoom change |
| UI Task (Core 1) | `drawBeaconProximityGauge()` | Every radar redraw |

Zoom-gating logic lives in `src/utils/task_manager.cpp:79-94`.

## Troubleshooting

### Arc gauge not appearing
- Verify zoom is set to 50m (beacon scanning only activates at 50m)
- Check beacon is in range: `beacon test` in serial console
- Confirm feature is enabled: `beacon enable on`
- Run `beacon scan` to list all visible BLE devices near you

### Sonar beeping but no arc gauge (or vice versa)
- Sound can be independently disabled in Settings > Sound > Beacon Sound
- Arc gauge is always shown when feature is active and beacon detected

### Distance reads very high or wrong
- Calibrate measured power: hold beacon exactly 1 meter away, run `beacon status`, note RSSI, then `beacon power -XX`
- Adjust path loss exponent for your environment: `beacon n 2.0` (open) to `beacon n 4.0` (dense indoor)

### Beacon detected but zone keeps jumping
- Run `beacon zone` to watch hysteresis state
- Reduce EMA alpha or increase hysteresis in `beacon_proximity.cpp` constants if environment is very noisy
- Common cause: reflections in indoor environments

### BLE scan interferes with WiFi
- BLE and WiFi share the 2.4GHz radio on ESP32-S3
- Beacon scanning is gated to 50m zoom to minimize overlap with WiFi scanning

## Performance

| Metric | Value |
|--------|-------|
| Scan CPU impact | Minimal (non-blocking BLE scan) |
| Arc draw time | < 1ms (single LVGL arc operation) |
| Memory (beacon_proximity module) | ~2KB RAM (state + trend history) |
| Flash impact | ~3,500 bytes |
| BLE scan current | +~20mA during active scan window |
| Scan duty cycle | 50% (1s scan / 2s interval) |

## Future Enhancements

- Multiple beacon tracking (show N nearest beacons simultaneously)
- UWB integration for centimeter-accurate ranging (requires hardware upgrade)
- Beacon direction estimation using RSSI differential from multiple scans
- Custom beacon name/label display on radar
- iBeacon / Eddystone UUID-based targeting (not just MAC)
- RSSI history graph in dev screen
