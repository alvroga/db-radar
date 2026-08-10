# Battery Monitoring System

**Waveshare ESP32-S3-Touch-LCD-2.1 Battery Management Guide**

---

## Hardware Overview

**Battery Connector**: MX1.25 2-pin (J1 on schematic), 3.7V Li-Ion/LiPo, 1500mAh recommended
(tested and verified capacity).

**Charging IC**: ETA6098 (U1), automatic charge management, up to 800mA via the ME6217C33M5G
regulator. LED1 is ON while charging, OFF when complete or no battery present.

**Voltage Monitoring**: GPIO4 (`BAT_ADC`), via a resistor divider (R5 200kΩ + R9 100kΩ, nominally
1:3). The 1:3 ratio is the hardware design value — the firmware uses a **calibrated** divider
constant instead of the theoretical 3.0, corrected against a real multimeter reading (see
Calibration below).

**Power Switch**: SW1 controls Q4 (a load switch) — must be ON for the battery to power the system;
OFF isolates the battery so it only charges.

```
USB_5V ──┬──> ETA6098 (Charging IC) ──> Battery (J1)
         │
         └──> ME6217C33M5G (3.3V Regulator) ──> 3V3

Battery ──> Q4 (Load Switch via SW1) ──> System Power
         └──> R5 (200K) ──> R9 (100K) ──> GPIO4 (BAT_ADC)
                              └──> GND
```

---

## Calibrated Constants

All values below are read directly from `src/hardware/sensors/battery.cpp`'s `config` namespace —
several are **calibrated from real measurement**, not the theoretical hardware values, so don't
"correct" them back toward round numbers without re-measuring first:

| Constant | Value | Note |
|---|---|---|
| `VOLTAGE_DIVIDER` | **3.255** | Calibrated: multimeter read 3.83V against a reported 3.53V → `3.0 × (3.83/3.53)`. Not the theoretical 1:3 (3.0) the resistor values alone would suggest. |
| `ADC_SAMPLES` | 15 | Averaged per reading |
| `EMA_ALPHA` | 0.2 | Smoothing factor on top of the averaged reading |
| `VBAT_MAX` | 4.12V | Fully charged, measured after USB disconnect (not the textbook 4.20V) |
| `VBAT_MIN` | 3.0V | Empty/cutoff |
| `VBAT_NOMINAL` | 3.7V | ~50% |
| `LOW_BATTERY_VOLTAGE` | 3.45V | ~20% warning threshold |
| `CRITICAL_VOLTAGE` | 3.36V | ~10% critical threshold |
| `SHUTDOWN_VOLTAGE` | 3.0V | 0% / emergency |

`VBAT_MAX`/thresholds were calibrated from a real discharge test (2026-02-13) — they're intentionally
not round numbers.

---

## Radar-Screen Display

**Current design**: an LVGL battery **icon** (not a percentage-text label), colored black in
daylight mode / white otherwise. Updated by the System Task, queued to the UI Task via
`BATTERY_UPDATE` (`src/utils/task_manager.cpp`), **throttled to every 30 seconds** — the icon only
has 5 visual states, so updating more often adds queue churn with no visible benefit. Raw ADC
sampling underneath still runs continuously; only the UI-visible update is throttled.

| Condition | Icon |
|---|---|
| ≥ 4.20V | Charging glyph (`LV_SYMBOL_CHARGE`) |
| > 87% | `LV_SYMBOL_BATTERY_FULL` |
| > 62% | `LV_SYMBOL_BATTERY_3` |
| > 37% | `LV_SYMBOL_BATTERY_2` |
| > 12% | `LV_SYMBOL_BATTERY_1` |
| ≤ 12% or invalid reading | `LV_SYMBOL_BATTERY_EMPTY` |

**Dev mode** (`settings.dev_mode`) shows `"%d%%\n%d.%02dV"` (percentage + voltage) instead of the
icon.

**Position**: `-150, 20` from the top-right (`src/ui/ui_manager.cpp`) — **not** a smaller offset.
The display is round, and anything closer to the edge than that clips or crashes near the circular
boundary; `-50px` was tried and cut the text off. This is the same class of constraint as the
waypoint off-screen-indicator boundary (see `docs/waypoint_filtering.md`'s FT-09 reference) — any
absolute-position UI element needs margin for the round display, not just the square framebuffer.

---

## Serial Commands

```
battery status            Voltage, percentage, source, charging state
battery voltage            Voltage only
battery percent            Percentage only
battery charging           Charging status
battery state               Charge state (CHARGING / FULL / NOT_CHARGING / UNKNOWN)
battery raw                 Raw ADC value
battery info                 Hardware configuration dump
battery monitor on|off      Periodic status logging
```

(`bat` works as a short alias for `battery` on all of the above.)

---

## API

Full function signatures live in `include/hardware/sensors/battery.h` — the high-level shape:

- `battery::init()` — sets up ADC on GPIO4; auto-initializes on first use if not called explicitly
- `battery::getVoltage()` / `battery::getRawADC()` — voltage in volts / raw 12-bit ADC value
- `battery::getPercent()` — linear interpolation between `VBAT_MIN` (0%) and `VBAT_MAX` (100%)
- `battery::getStatus()` — returns a `BatteryStatus` struct (voltage, percent, power source, charge
  state, raw ADC, validity flag) — this is what the display and serial commands both read from
- `battery::isLowBattery()` / `battery::isCriticalBattery()` — threshold checks against the
  calibrated constants above
- `battery::checkBatteryWarnings()` — call periodically; logs low/critical warnings at throttled
  intervals

---

## Troubleshooting

### Battery not charging (LED1 stays off with USB connected)
1. **Battery already full** — LED1 off at full charge is normal, not a fault.
2. **Protection circuit asleep** — disconnect and reconnect the battery, wait ~10s.
3. **Connector not fully seated** — check polarity (red = +, black = −) and that it clicks in.
4. **Deep-discharged battery** (<2.5V) — leave on USB 30-60 minutes; ETA6098 has a trickle-charge
   mode for this case.

### Incorrect/unstable voltage readings
1. Confirm `battery::init()` ran (auto-inits on first use if not).
2. Confirm nothing else is using GPIO4.
3. If readings are consistently off by a fixed amount, the `VOLTAGE_DIVIDER` constant may need
   re-calibrating the same way it originally was — measure actual voltage with a multimeter,
   compare against `battery voltage`'s reading, and scale the constant by the ratio. Don't guess at
   a new value without a real measurement.

### Device doesn't run on battery with USB disconnected
1. Confirm SW1 is in the ON position — OFF isolates the battery to charge-only.
2. Check `battery voltage` — below ~3.0V the battery may be too discharged to power the system.
3. Some batteries' protection circuits cut output below ~2.5V or on overcurrent — try a different
   battery to rule this out.

---

## Related

Low-power behavior (what happens at low battery, standby entry/exit, GPS power modes) lives in
[`docs/standby_mode.md`](standby_mode.md), not here — this doc covers measurement and display only.

---

## Reference Links

- [Waveshare ESP32-S3-Touch-LCD-2.1 Product Page](https://www.waveshare.com/esp32-s3-touch-lcd-2.1.htm)
- [ETA6098 Datasheet](https://pdf1.alldatasheet.com/datasheet-pdf/view/1132890/ETA/ETA6098.html)
- [ESP32-S3 ADC Documentation](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/adc.html)
