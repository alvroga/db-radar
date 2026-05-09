# Battery Monitoring System Documentation

**Waveshare ESP32-S3-Touch-LCD-2.1 Battery Management Guide**

---

## Table of Contents

1. [Hardware Overview](#hardware-overview)
2. [Features](#features)
3. [Getting Started](#getting-started)
4. [Serial Commands](#serial-commands)
5. [API Reference](#api-reference)
6. [Battery Specifications](#battery-specifications)
7. [Troubleshooting](#troubleshooting)
8. [Advanced Topics](#advanced-topics)

---

## Hardware Overview

### Battery Management Hardware

The Waveshare ESP32-S3-Touch-LCD-2.1 includes professional battery management hardware:

**Battery Connector**:
- **Type**: MX1.25 2-pin connector (J1 on schematic)
- **Voltage**: 3.7V Li-Ion/LiPo batteries
- **Recommended Capacity**: 1500mAh (tested and verified)
- **Protection**: Integrated protection circuit recommended

**Charging IC**:
- **Model**: ETA6098 (U1 on schematic)
- **Features**: Automatic charge management, overcharge protection
- **Charge Current**: Up to 800mA (via ME6217C33M5G regulator)
- **Charge Indicator**: LED1 (ON = charging, OFF = complete or no battery)

**Voltage Monitoring**:
- **GPIO**: GPIO4 connected to BAT_ADC signal
- **Voltage Divider**: R5 (200kΩ) + R9 (100kΩ) = 1:3 ratio
- **Measurement Range**: 1.0V - 1.4V (ADC input after divider)
- **Battery Range**: 3.0V - 4.2V (actual battery voltage)

**Power Switch**:
- **Component**: SW1 controls Q4 (DMP2066LSN load switch)
- **Function**: Enable/disable battery power output
- **Position**: Must be ON for battery operation

### Hardware Schematic Details

```
USB_5V ──┬──> ETA6098 (Charging IC) ──> Battery (J1)
         │
         └──> ME6217C33M5G (3.3V Regulator) ──> 3V3

Battery ──> Q4 (Load Switch via SW1) ──> System Power
         └──> R5 (200K) ──> R9 (100K) ──> GPIO4 (BAT_ADC)
                              └──> GND
```

**Key Points**:
- Battery charges when USB connected (automatic via ETA6098)
- SW1 must be ON for battery to power the system
- GPIO4 reads 1/3 of battery voltage (due to voltage divider)
- LED1 indicates charging status (hardware-controlled)

---

## Features

### Real-Time Monitoring
- ✅ Accurate battery voltage reading (±0.05V)
- ✅ Battery percentage calculation (0-100%)
- ✅ Power source detection (USB vs Battery)
- ✅ Charging state detection (Charging/Full/Not Charging)
- ✅ Raw ADC value access for debugging

### Visual Display (NEW - 2025-10-15)
- ✅ **Always-visible percentage on radar screen** (top-right corner)
- ✅ **Color-coded status**: Green (>70%) → Yellow (50-70%) → Red (<50%)
- ✅ **Auto-updates every 5 seconds** via System Task
- ✅ **Simple format**: Just percentage ("69%"), no voltage clutter
- ✅ **Independent of serial monitoring** - Always visible

### Smart Warnings
- ✅ Low battery warning at 20% (3.5V)
- ✅ Critical battery warning at 10% (3.3V)
- ✅ Configurable warning intervals
- ✅ Automatic warnings when on battery power

### Diagnostic Tools
- ✅ 8 serial commands for complete control
- ✅ Periodic monitoring mode (60-second intervals)
- ✅ Hardware configuration reporting
- ✅ Integration with existing diagnostics system

---

## Getting Started

### 1. Hardware Setup

**Connect Battery**:
1. Ensure USB is connected (powers the board)
2. Connect 3.7V Li-Ion battery to J1 (MX1.25 connector)
   - Red wire = Positive (+)
   - Black wire = Negative (-)
3. Battery will start charging automatically (LED1 turns ON)

**Enable Battery Power**:
1. Locate SW1 (power switch) on the board
2. Switch SW1 to ON position
3. Battery can now power the system when USB is disconnected

**Test Battery Operation**:
1. With switch ON and battery connected
2. Disconnect USB cable
3. Device should remain powered (running on battery)
4. Reconnect USB to resume charging

### 2. Software Setup

The battery monitoring system initializes automatically when you include it in your code.

**Option A: Automatic Initialization** (Recommended)
```cpp
// In main.cpp setup()
#include "hardware/sensors/battery.h"

void setup() {
    Serial.begin(115200);

    // Initialize battery monitoring
    battery::init();

    // Battery monitoring is now active
    // Use serial commands to interact
}

void loop() {
    // Optional: Update periodic monitoring
    battery::updatePeriodicMonitoring();

    // Optional: Check warnings
    battery::checkBatteryWarnings();

    delay(100);
}
```

**Option B: On-Demand Monitoring**
```cpp
// No automatic initialization needed
// Battery functions will auto-initialize on first use

void checkBatteryStatus() {
    float voltage = battery::getVoltage();
    int percent = battery::getPercent();

    Serial.printf("Battery: %.2fV (%d%%)\n", voltage, percent);
}
```

### 3. Testing Battery Monitoring

**Using Serial Commands** (115200 baud):

1. **Open serial monitor**:
   ```bash
   pio device monitor
   ```

2. **Check battery status**:
   ```
   battery status
   ```
   Expected output:
   ```
   [BATTERY] Voltage: 4.15V | 95% | Source: USB | Charging: Yes
   ```

3. **Test power source detection**:
   - With USB connected: `Source: USB`
   - Unplug USB: `Source: Battery`
   - Plug USB back: `Source: USB | Charging: Yes`

4. **Enable periodic monitoring**:
   ```
   battery monitor on
   ```
   Status will be logged every 60 seconds automatically.

---

## Serial Commands

### Command Reference

| Command | Short | Description | Example Output |
|---------|-------|-------------|----------------|
| `battery status` | `bat status` | Complete battery status | `Voltage: 3.87V \| 65% \| Source: USB \| Charging: Yes` |
| `battery voltage` | `bat voltage` | Battery voltage only | `Voltage: 3.87V` |
| `battery percent` | `bat percent` | Battery percentage only | `Battery: 65%` |
| `battery charging` | `bat charging` | Charging status | `Charging: Yes` |
| `battery source` | `bat source` | Power source | `Power Source: USB` |
| `battery raw` | `bat raw` | Raw ADC value | `Raw ADC: 1523 / 4095` |
| `battery info` | `bat info` | Hardware configuration | See example below |
| `battery monitor on` | `bat monitor on` | Enable periodic logging | `Periodic monitoring enabled (interval: 60 seconds)` |
| `battery monitor off` | `bat monitor off` | Disable periodic logging | `Periodic monitoring disabled` |

### Command Examples

**Basic Status Check**:
```
> battery status
[BATTERY] Voltage: 3.87V | 65% | Source: Battery | Not Charging
```

**Hardware Information**:
```
> battery info
[BATTERY] Hardware Configuration:
  GPIO Pin: 4
  Voltage Divider: 1:3.0 (R5=200K, R9=100K)
  Voltage Range: 3.0V - 4.2V
  ADC Resolution: 12-bit (0-4095)
  ADC Reference: 3.3V
  Samples per Reading: 10
```

**Periodic Monitoring**:
```
> battery monitor on
[BATTERY] Periodic monitoring enabled (interval: 60 seconds)
[BATTERY] 4.15V (95%) | USB | Charging
... (60 seconds later)
[BATTERY] 4.18V (98%) | USB | Charging
... (60 seconds later)
[BATTERY] 4.20V (100%) | USB | Full
```

---

## API Reference

### Initialization

```cpp
bool battery::init();
```
Initialize battery monitoring system. Sets up ADC on GPIO4 with proper configuration.
- **Returns**: `true` if successful, `false` on error
- **Note**: Auto-initializes on first use if not called explicitly

### Voltage Reading

```cpp
float battery::getVoltage();
```
Read current battery voltage.
- **Returns**: Battery voltage in volts (3.0V - 4.2V range), or -1.0 on error
- **Accuracy**: ±0.05V with 10-sample averaging

```cpp
uint16_t battery::getRawADC();
```
Read raw 12-bit ADC value from GPIO4.
- **Returns**: ADC value (0-4095), or 0 on error
- **Use case**: Debugging, calibration

### Percentage Calculation

```cpp
int battery::getPercent();
int battery::getPercent(float voltage);
```
Calculate battery percentage from voltage.
- **Returns**: Battery percentage (0-100%), or -1 on error
- **Algorithm**: Linear interpolation between 3.0V (0%) and 4.2V (100%)

### Status Functions

```cpp
battery::BatteryStatus battery::getStatus();
```
Get complete battery status information.
- **Returns**: BatteryStatus structure with all fields populated

```cpp
struct BatteryStatus {
    float voltage;              // Battery voltage (V)
    int percent;                // Percentage (0-100%)
    PowerSource power_source;   // USB or BATTERY
    ChargeState charge_state;   // CHARGING, FULL, NOT_CHARGING, or UNKNOWN
    uint16_t adc_raw;          // Raw ADC reading
    bool valid;                 // True if reading is valid
};
```

```cpp
battery::PowerSource battery::getPowerSource();
```
Detect current power source.
- **Returns**: `PowerSource::USB` or `PowerSource::BATTERY`
- **Detection**: Based on voltage (>4.0V = USB, <4.0V = Battery)

```cpp
battery::ChargeState battery::getChargeState();
```
Determine charging state.
- **Returns**: `ChargeState::CHARGING`, `FULL`, `NOT_CHARGING`, or `UNKNOWN`

```cpp
bool battery::isCharging();
bool battery::isOnBattery();
bool battery::isUSBConnected();
```
Convenience functions for quick checks.

### Warning Functions

```cpp
void battery::checkBatteryWarnings();
```
Check battery level and issue warnings if needed.
- **Low Battery** (<3.5V / 20%): Warning every 5 minutes
- **Critical** (<3.3V / 10%): Warning every 1 minute
- **Call from**: Main loop or system task (every 30 seconds recommended)

```cpp
bool battery::isLowBattery();
bool battery::isCriticalBattery();
```
Check if battery is at warning thresholds.
- **Returns**: `true` if below threshold AND on battery power

### Monitoring Functions

```cpp
void battery::setPeriodicMonitoring(bool enable);
```
Enable/disable automatic status logging every 60 seconds.

```cpp
void battery::updatePeriodicMonitoring();
```
Update periodic monitoring (call from main loop).

```cpp
void battery::printHardwareInfo();
void battery::printStatus();
```
Print detailed information to serial console.

### Utility Functions

```cpp
const char* battery::powerSourceToString(PowerSource source);
const char* battery::chargeStateToString(ChargeState state);
```
Convert enums to human-readable strings.

---

## Battery Specifications

### Recommended Battery

**Tested Battery**:
- [Amazon Link](https://www.amazon.com/Rechargeable-Integrated-Protection-Development-Electronic/dp/B0DSMRNHSP)
- **Type**: 18650 Li-Ion with integrated protection
- **Voltage**: 3.7V nominal (3.0V - 4.2V range)
- **Capacity**: 1500mAh
- **Protection**: Overcharge, over-discharge, short-circuit

### Li-Ion Voltage Curve

| Voltage | Percentage | Status | Runtime Estimate* |
|---------|-----------|--------|------------------|
| **4.20V** | 100% | Fully charged | ~10 hours |
| **4.10V** | 90% | Excellent | ~9 hours |
| **4.00V** | 80% | Very good | ~8 hours |
| **3.90V** | 70% | Good | ~7 hours |
| **3.80V** | 60% | Good | ~6 hours |
| **3.70V** | 50% | Nominal | ~5 hours |
| **3.60V** | 40% | Fair | ~4 hours |
| **3.50V** | 20% | ⚠️ Low | ~2 hours |
| **3.40V** | 15% | ⚠️ Low | ~1.5 hours |
| **3.30V** | 10% | ⚠️ Critical | ~1 hour |
| **3.20V** | 5% | 🔴 Very low | ~30 min |
| **3.00V** | 0% | 🔴 Empty | Shutdown |

*Estimates based on 1500mAh battery with ~150mA average current draw (GPS radar application)

### Charging Specifications

| Parameter | Value | Notes |
|-----------|-------|-------|
| **Charge Current** | Up to 800mA | Via ME6217C33M5G regulator |
| **Charge Time** | ~2 hours | From empty to 100% (1500mAh) |
| **Charge Method** | CC/CV (Constant Current/Constant Voltage) | Standard Li-Ion charging |
| **Charge Termination** | 4.2V ± 0.05V | Automatic via ETA6098 |
| **Trickle Charge** | Yes | For deep-discharged batteries |
| **Temperature Range** | 0°C to 45°C | For safe charging |

### Power Consumption Estimates

| Mode | Current Draw | Battery Life (1500mAh) |
|------|-------------|------------------------|
| **GPS Radar Active** | ~150mA | ~10 hours |
| **Display On, GPS Off** | ~80mA | ~18 hours |
| **Idle (Display Off)** | ~20mA | ~75 hours |
| **Deep Sleep** | <1mA | ~1500 hours |

---

## Battery Display on Radar Screen

### Overview

**NEW (2025-10-15)**: The radar screen now includes a simple, always-visible battery percentage indicator in the top-right corner.

**Design Philosophy**:
- **Simple**: Just percentage, no voltage or charging state
- **Always visible**: Updates every 5 seconds, independent of serial monitoring
- **Color-coded**: Visual feedback at a glance (Green/Yellow/Red)
- **Uncluttered**: Minimal space usage, doesn't distract from radar

###Display Layout

```
┌─────────────────────────────────┐
│                            69%  │ ← Battery percentage (Yellow @ 69%)
│                                 │
│                                 │
│         RADAR DISPLAY           │
│          (Green)                │
│                                 │
│                                 │
│    GPS: Fixed (6 sats)          │ ← GPS status (bottom-center)
└─────────────────────────────────┘
```

### Color Coding

| Battery % | Color  | Hex Code | Visual Meaning |
|-----------|--------|----------|----------------|
| **> 70%** | Green  | 0x00FF00 | Good - no action needed |
| **50-70%** | Yellow | 0xFFFF00 | Moderate - consider charging |
| **< 50%** | Red    | 0xFF0000 | Low - charge soon |

### Implementation Details

**Position**: `-150, 20` (pixels from right/top edges)

**Why -150px?**
The radar uses a circular clipping stage (`LV_RADIUS_CIRCLE`) which clips more aggressively at edges than square displays.

**Evolution of positioning**:
1. ❌ `-10px` → Text completely off-screen, system crash
2. ❌ `-50px` → Text cutoff ("Ba" instead of "69%")
3. ✅ `-150px` → Full text visible, safe margin

**Key Code**:
```cpp
// UI Creation (src/ui/ui_manager.cpp:129-135)
g_ui_state.battery_label = lv_label_create(stage);
lv_obj_set_style_text_color(g_ui_state.battery_label, lv_color_hex(0x00FF00), 0);
lv_obj_set_style_text_font(g_ui_state.battery_label, &lv_font_montserrat_14, 0);
lv_label_set_text(g_ui_state.battery_label, "--%");
lv_obj_align(g_ui_state.battery_label, LV_ALIGN_TOP_RIGHT, -150, 20);  // CRITICAL: -150!

// Update Logic (src/utils/task_manager.cpp:695-714)
battery::BatteryStatus bat_status = battery::getStatus();
lv_label_set_text_fmt(ui.battery_label, "%d%%", bat_status.percent);

// Color coding
if (bat_status.percent > 70) {
    lv_obj_set_style_text_color(ui.battery_label, lv_color_hex(0x00FF00), 0);  // Green
} else if (bat_status.percent >= 50) {
    lv_obj_set_style_text_color(ui.battery_label, lv_color_hex(0xFFFF00), 0);  // Yellow
} else {
    lv_obj_set_style_text_color(ui.battery_label, lv_color_hex(0xFF0000), 0);  // Red
}
```

### Two Independent Systems

**1. Battery Monitoring** (`battery::update()`):
- Collects voltage samples via GPIO4 ADC
- Tracks voltage history (100 samples)
- Performs trend analysis (charging/discharging/stable)
- Provides serial diagnostics
- Can be controlled via `battery monitor on|off`

**2. Battery Display** (`ui.battery_label`):
- Visual percentage on radar screen
- Gets data from `battery::getStatus()`
- Always updates every 5 seconds
- **Independent** of serial monitoring setting

**Important**: Disabling serial monitoring (`battery monitor off`) does NOT disable the UI display!

### FAQs

**Q: Can I hide the battery display?**

A: Yes, modify `src/ui/ui_manager.cpp`:
```cpp
// Option 1: Don't create (comment out lines 129-135)

// Option 2: Hide after creation (in task_manager.cpp)
lv_obj_add_flag(ui.battery_label, LV_OBJ_FLAG_HIDDEN);
```

**Q: Can I change the color thresholds?**

A: Yes, modify `src/utils/task_manager.cpp`:
```cpp
// Current: 70% and 50%
if (bat_status.percent > 80) {      // Change from 70
    // Green
} else if (bat_status.percent >= 40) {  // Change from 50
    // Yellow
} else {
    // Red
}
```

**Q: Can I add voltage to the display?**

A: Yes, change format in `task_manager.cpp`:
```cpp
// Current: percentage only
lv_label_set_text_fmt(ui.battery_label, "%d%%", bat_status.percent);

// New: voltage + percentage
lv_label_set_text_fmt(ui.battery_label, "%.1fV %d%%", bat_status.voltage, bat_status.percent);
```

**Warning**: Longer text may require further position adjustment!

**Q: Can I show charging icon?**

A: Yes, but requires creating/adding icon assets:
```cpp
// 1. Create battery icon images (see LVGL image converter)
LV_IMG_DECLARE(battery_charging);
LV_IMG_DECLARE(battery_full);

// 2. Use image instead of label
g_ui_state.battery_icon = lv_img_create(stage);
lv_img_set_src(ui.battery_icon, &battery_full);

// 3. Update based on status
if (bat_status.charge_state == battery::ChargeState::CHARGING) {
    lv_img_set_src(ui.battery_icon, &battery_charging);
}
```

---

## Troubleshooting

### Battery Not Charging

**LED1 stays OFF when USB connected**:

1. **Battery already full**: This is normal! LED1 turns OFF when charge is complete.
   - **Test**: Disconnect USB, run device for 15 minutes, reconnect USB
   - **Expected**: LED1 should turn ON if battery discharged

2. **Battery protection circuit in sleep mode**:
   - **Solution**: Disconnect and reconnect battery
   - **Wait**: 10 seconds before reconnecting
   - **Watch**: LED1 should blink momentarily when protection circuit wakes up

3. **Connector not fully seated**:
   - **Check**: Connector should "click" when properly inserted
   - **Verify**: Red wire = (+), Black wire = (-)
   - **Inspect**: Pins not bent or damaged

4. **Battery voltage too low** (<2.5V):
   - **Solution**: Leave connected to USB for 30-60 minutes
   - **ETA6098**: Has trickle charge mode for deep-discharged batteries
   - **Monitor**: LED1 may eventually turn ON

### Incorrect Voltage Readings

**Voltage reading is wrong or unstable**:

1. **Check initialization**:
   ```cpp
   battery::init();  // Call once during setup
   ```

2. **Verify GPIO4 is not used elsewhere**:
   - GPIO4 is dedicated to BAT_ADC
   - Check for conflicts in your code

3. **ADC calibration** (if consistently off):
   ```cpp
   // Measure actual battery voltage with multimeter
   float actual_voltage = 3.85;  // Example reading
   float measured_voltage = battery::getVoltage();
   float error = actual_voltage - measured_voltage;

   // If error > 0.1V, may need calibration adjustment
   Serial.printf("Calibration error: %.2fV\n", error);
   ```

4. **Power supply noise**:
   - Voltage can fluctuate by ±0.05V during high-current operations
   - Use `battery monitor on` to observe trends over time

### Power Switch Issues

**Device doesn't run on battery when USB disconnected**:

1. **SW1 position**: Ensure power switch is ON
   - OFF = Battery isolated (only charges)
   - ON = Battery can power system

2. **Battery voltage too low**:
   - Check voltage: `battery voltage`
   - If <3.0V, battery may be too discharged to power system
   - Charge battery fully before testing

3. **Battery protection circuit**:
   - Some batteries have protection circuits that disable output if:
     - Voltage too low (<2.5V)
     - Current too high (short circuit)
   - **Solution**: Try a different battery

### Warning Messages

**Getting low battery warnings when battery is full**:

1. **Check power source detection**:
   ```
   battery source
   ```
   - Should show "USB" when connected
   - If shows "Battery" when USB connected, voltage may be borderline (~4.0V)

2. **Verify battery voltage**:
   ```
   battery voltage
   ```
   - >4.0V = USB power (no warnings)
   - <4.0V = Battery power (warnings active)

---

## Advanced Topics

### Integrating with FreeRTOS Tasks

**System Task Integration** (task_manager.cpp):

```cpp
void systemTask(void* param) {
    while (true) {
        // Check battery warnings every 30 seconds
        battery::checkBatteryWarnings();

        // Check if critical battery
        if (battery::isCriticalBattery()) {
            // Take action: reduce power consumption
            // Example: Disable GPS, dim display, etc.
        }

        vTaskDelay(pdMS_TO_TICKS(30000));  // 30 seconds
    }
}
```

### Custom Warning Thresholds

Modify `src/hardware/sensors/battery.cpp`:

```cpp
namespace config {
    // Change these values for custom thresholds
    constexpr float LOW_BATTERY_VOLTAGE = 3.6;    // Was 3.5V
    constexpr float CRITICAL_VOLTAGE = 3.4;       // Was 3.3V

    // Change warning intervals
    constexpr unsigned long LOW_BATTERY_WARNING_INTERVAL = 180000;  // Was 5 min, now 3 min
    constexpr unsigned long CRITICAL_WARNING_INTERVAL = 30000;      // Was 1 min, now 30 sec
}
```

### Battery Capacity Estimation

**Coulomb Counting Approach** (future enhancement):

```cpp
// Pseudocode for future implementation
class BatteryMonitor {
    float capacity_mah = 1500.0;  // Battery capacity
    float remaining_mah = 1500.0;  // Current remaining capacity

    void updateCapacity(float current_ma, uint32_t time_ms) {
        // Integrate current over time
        float charge_used = (current_ma * time_ms) / 3600000.0;  // mAh
        remaining_mah -= charge_used;

        // Calculate percentage from coulomb counting
        int percent = (int)((remaining_mah / capacity_mah) * 100.0);
    }
};
```

### Power Optimization

**Reduce Power Consumption**:

1. **Display backlight**: Biggest power consumer
   ```cpp
   backlight::setBrightness(50);  // Reduce from 78% to 50%
   // Saves ~30-40mA
   ```

2. **GPS update rate**: Second biggest consumer
   ```cpp
   gps_lc76g::setUpdateRate(5000);  // Reduce from 1Hz to 0.2Hz
   // Saves ~10-15mA
   ```

3. **WiFi/BLE**: Disable when not needed
   ```cpp
   scanner::setWiFiEnabled(false);
   scanner::setBLEEnabled(false);
   // Saves ~50-80mA
   ```

4. **Deep sleep mode** (future):
   ```cpp
   // When battery < 10%, enter deep sleep
   if (battery::isCriticalBattery()) {
       esp_sleep_enable_timer_wakeup(300 * 1000000);  // Wake every 5 min
       esp_deep_sleep_start();
   }
   ```

### Calibration Procedure

**ADC Calibration** (if needed):

1. **Measure actual battery voltage** with multimeter:
   ```
   Multimeter reading: 3.85V
   ```

2. **Check ADC reading**:
   ```
   battery voltage
   [BATTERY] Voltage: 3.80V
   ```

3. **Calculate error**:
   ```
   Error = 3.85V - 3.80V = +0.05V
   ```

4. **Adjust voltage divider constant** (if error > 0.1V):

   In `battery.cpp`:
   ```cpp
   namespace config {
       // Original: 3.0 (theoretical)
       // Calibrated: 3.0 * (1 + error_percent)
       constexpr float VOLTAGE_DIVIDER = 3.04;  // Adjusted from 3.0
   }
   ```

5. **Re-test** and verify accuracy.

---

## Future Enhancements

### Planned Features (v1.0+)

**UI Integration**:
- Battery icon in status bar
- Charging animation
- Visual low battery warnings
- Settings screen battery tab

**Advanced Monitoring**:
- Battery cycle counting (charge/discharge cycles)
- Battery health estimation (capacity degradation)
- Charge time remaining calculation
- Power consumption profiling

**Data Logging**:
- Battery usage statistics (mAh consumed)
- Voltage history graph
- Charge/discharge curves
- NVS storage of battery history

**Power Management**:
- Automatic power-saving modes
- Smart charging (stop at 80% for longevity)
- Temperature monitoring (if sensor added)
- Battery health notifications

---

## Reference Links

**Hardware**:
- [Waveshare ESP32-S3-Touch-LCD-2.1 Product Page](https://www.waveshare.com/esp32-s3-touch-lcd-2.1.htm)
- [Recommended Battery (Amazon)](https://www.amazon.com/Rechargeable-Integrated-Protection-Development-Electronic/dp/B0DSMRNHSP)
- [ETA6098 Datasheet](https://pdf1.alldatasheet.com/datasheet-pdf/view/1132890/ETA/ETA6098.html)

**Software**:
- [ESP32-S3 ADC Documentation](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/adc.html)
- [Li-Ion Battery Basics](https://batteryuniversity.com/article/bu-204-how-do-lithium-batteries-work)

---

## Support

**Issues**:
- Open GitHub issue for bugs or feature requests
- Include battery voltage reading, percentage, and power source in report

**Questions**:
- Use GitHub Discussions for general questions
- Include `battery info` output when asking for help

---

**Last Updated**: 2025-10-14
**Version**: 1.0.0
**Status**: Production Ready
**Tested Battery**: 1500mAh 3.7V Li-Ion with protection circuit
