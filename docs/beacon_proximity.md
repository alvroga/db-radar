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

### RSSI-to-Arc Mapping

The arc angle is linearly interpolated from the EMA-smoothed RSSI value:

| EMA RSSI | Arc Angle | Typical Distance |
|----------|-----------|-----------------|
| ≤ -90 dBm | 0° (hidden) | Out of range |
| -90 dBm | 0° | ~10m+ |
| -45 dBm | 355° (full) | ~0.5m |

Formula: `arc_degrees = clamp((rssi + 90) / 45.0 * 355, 0, 355)`

### Code Reference

- Drawing: `src/ui/navigation.cpp` — `drawBeaconProximityGauge()` (~line 390-420)
- Called from: `src/ui/navigation.cpp` — `updateRadarDisplay()` (last draw layer)

## Audio: Sonar Beeping

The buzzer pulses at variable intervals based on EMA RSSI, giving an audio sonar effect:

| EMA RSSI | Beep Interval | Sensation |
|----------|--------------|-----------|
| < -85 dBm | Silent | Out of range |
| -85 to -75 dBm | 1800ms | Slow — far away |
| -75 to -65 dBm | 900ms | Medium |
| -65 to -55 dBm | 500ms | Fast |
| ≥ -55 dBm | 200ms | Rapid — very close (<1m) |

Beeping can be independently enabled/disabled in Settings (Settings > Sound > Beacon Sound toggle) without disabling the visual arc gauge.

### Code Reference

- Sonar update: `src/hardware/connectivity/beacon_proximity.cpp` — `updateSonar()` (~line 398-416)
- Buzzer API: `include/hardware/buzzer.h` — `setSonarInterval()`, `stopSonar()`
- Buzzer state machine: `src/hardware/buzzer.cpp` (~line 164-217)

## RSSI Processing

Raw BLE RSSI readings are noisy. The system applies two layers of smoothing and stability logic:

### EMA Smoothing (α = 0.4)

Each scan result is blended into a running average:
```
rssi_ema = α × rssi_raw + (1 - α) × rssi_ema_prev
```
- α = 0.4 gives moderate smoothing — responsive but not jittery
- Both the arc gauge and sonar use `rssi_ema`, never raw RSSI

### Zone Detection with Hysteresis

Zones prevent rapid oscillation at zone boundaries:

| Zone | RSSI Threshold | Beep Rate |
|------|---------------|-----------|
| OUT_OF_RANGE | < -85 dBm | Silent |
| FAR | -85 to -75 dBm | 1800ms |
| MEDIUM | -75 to -65 dBm | 900ms |
| CLOSE | ≥ -65 dBm | 500ms |

- **Hysteresis**: ±3 dBm band — must exceed threshold by 3 dBm to change zones
- **Confirmation**: Requires 2 consecutive readings in new zone before confirming change

### Movement Trend Detection

Linear regression on last 10 EMA samples computes a slope:
- `slope > +2 dBm/cycle` → APPROACHING
- `slope < -2 dBm/cycle` → DEPARTING
- `|slope| < 2 dBm/cycle` → STABLE

Available via serial: `beacon trend`

## BLE Scanning

- **Scan mode**: Active (full power, ~20mA additional)
- **Scan duration**: 1 second per scan
- **Scan interval**: Every 2 seconds (50% duty cycle)
- **Filter**: Target MAC address (case-insensitive match)
- **Early stop**: Scan stops immediately when target MAC is found
- **Timeout**: Beacon marked lost after 15 seconds without detection

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
