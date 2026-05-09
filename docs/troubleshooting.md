# Troubleshooting Guide

## Common Issues and Solutions

### **Display Issues**

#### Display Jitter/Flicker
**Symptoms**: Screen flickering or unstable display
**Causes**:
- Incorrect timing parameters
- PCLK frequency too high
- Poor power supply

**Solutions**:
```cpp
// Use stable timing baseline
cfg.timings.pclk_hz = 10000000;  // Reduce from 12MHz
cfg.timings.hsync_back_porch = 20;   // Increase from 16
cfg.timings.hsync_front_porch = 20;  // Increase from 16
cfg.timings.vsync_front_porch = 10;  // Increase from 8
```

#### No Display Output
**Symptoms**: Blank screen, backlight may work
**Causes**:
- LCD_CS not properly controlled
- SPI initialization failed
- RGB timing misconfiguration

**Solutions**:
1. Verify LCD_CS is held HIGH after init
2. Check SPI command sequence
3. Monitor serial output for timing confirmation

#### Color Issues
**Symptoms**: Wrong colors, color bleeding
**Causes**:
- Incorrect RGB pin mapping
- Signal integrity issues

**Solutions**:
```cpp
// Verify DATA_PINS array
static const int DATA_PINS[16] = {
    5,45,48,47,21,14,13,12,11,10,9,46,3,8,18,17
};
```

### **I2C Communication Errors**

#### Frequent Bus Errors
**Symptoms**:
```
[Wire.cpp:499] requestFrom(): i2cWriteReadNonStop returned Error -1
```

**Causes**:
- Device not connected
- Bus speed too high
- Multiple I2C implementations conflict
- Insufficient pull-up resistors

**Solutions**:
1. **Reduce I2C frequency**:
```cpp
Wire.begin(I2C_SDA, I2C_SCL, 100000);  // Reduce from 400kHz
```

2. **Implement throttling**:
```cpp
static uint32_t last_read = 0;
if (millis() - last_read >= 5000) {
    // Perform I2C operation
    last_read = millis();
}
```

3. **Check device presence**:
```cpp
bool isDevicePresent(uint8_t addr) {
    Wire.beginTransmission(addr);
    return (Wire.endTransmission() == 0);
}
```

#### Device Not Responding
**Symptoms**: Specific I2C device timeouts
**Causes**:
- Device not connected
- Wrong I2C address
- Power supply issues

**Solutions**:
1. **I2C Scanner**:
```cpp
void scanI2C() {
    for (uint8_t addr = 1; addr < 127; addr++) {
        Wire.beginTransmission(addr);
        if (Wire.endTransmission() == 0) {
            Serial.printf("Device found at 0x%02X\n", addr);
        }
    }
}
```

2. **Verify addresses**:
- Touch (CST820): 0x15
- RTC (PCF85063): 0x51
- IO Expander (TCA9554): 0x20
- IMU (QMI8658): 0x6A or 0x6B

### **Touch Issues**

#### Touch Not Responding
**Symptoms**: No touch events registered
**Causes**:
- I2C communication failure
- Wrong touch mapping
- Interrupt pin not configured

**Solutions**:
1. **Verify I2C communication**
2. **Check touch mapping**:
```cpp
// CST820 configuration
data->point.x = touch_x;  // swap_xy = false
data->point.y = 480 - touch_y;  // inv_y = true
```

#### Inaccurate Touch
**Symptoms**: Touch offset or wrong coordinates
**Causes**:
- Incorrect coordinate mapping
- Need calibration

**Solutions**:
```cpp
// Fine-tune touch mapping in cst820.cpp
// Test with touch debug output enabled
```

### **WiFi/BLE Issues**

#### WiFi Scanning Shows 0 Networks
**Symptoms**: WiFi count always 0
**Causes**:
- WiFi not properly initialized
- Scan timing issues
- Radio interference

**Solutions**:
```cpp
// Verify initialization order
WiFi.mode(WIFI_STA);
WiFi.disconnect(true);
WiFi.scanDelete();
```

#### BLE Scanning Not Working
**Symptoms**: BLE count always 0
**Causes**:
- Wrong BLE library (NimBLE vs standard)
- Scan parameters incorrect
- Radio interference

**Solutions**:
1. **Use standard ESP32 BLE**:
```cpp
#include <BLEDevice.h>  // Not NimBLE
```

2. **Verify scan parameters**:
```cpp
pScan->setActiveScan(false);  // Passive scan
pScan->setInterval(160);
pScan->setWindow(80);
```

### **Memory Issues**

#### Heap Fragmentation
**Symptoms**: Random crashes, malloc failures
**Causes**:
- Memory leaks
- Large allocations fragmenting heap
- Insufficient PSRAM usage

**Solutions**:
1. **Monitor heap**:
```cpp
Serial.printf("Free heap: %d, PSRAM: %d\n",
              ESP.getFreeHeap(), ESP.getFreePsram());
```

2. **Use PSRAM for large buffers**:
```cpp
cfg.flags.fb_in_psram = 1;
```

#### Stack Overflow
**Symptoms**: Core panic, stack traces
**Causes**:
- Large local variables
- Deep function recursion
- Insufficient task stack size

**Solutions**:
```cpp
// Increase task stack size
xTaskCreatePinnedToCore(task_func, "task", 8192, nullptr, 1, nullptr, 0);
```

### **Performance Issues**

#### Low Frame Rate
**Symptoms**: Sluggish UI, low FPS
**Causes**:
- Blocking I2C operations
- Inefficient LVGL configuration
- CPU-intensive operations in main loop

**Solutions**:
1. **Throttle I2C operations**
2. **Optimize LVGL**:
```cpp
cfg.bounce_buffer_size_px = 10 * 480;
cfg.psram_trans_align = 64;
```
3. **Move heavy operations to tasks**

#### High Memory Usage
**Symptoms**: Out of memory errors
**Causes**:
- LVGL objects not cleaned up
- Large static buffers
- Memory fragmentation

**Solutions**:
1. **Clean up LVGL objects**:
```cpp
lv_obj_del(object);
```
2. **Use dynamic allocation wisely**
3. **Monitor memory usage regularly**

### **Development Issues**

#### Upload Failures
**Symptoms**: Cannot upload firmware
**Causes**:
- Wrong upload speed
- USB connection issues
- Board in wrong mode

**Solutions**:
1. **Reduce upload speed**: 460800 → 115200
2. **Try different USB cables**
3. **Hold GPIO0 during reset** (if needed)

#### Serial Monitor Not Working
**Symptoms**: No serial output
**Causes**:
- Wrong baud rate
- USB CDC issues
- Driver problems

**Solutions**:
1. **Verify baud rate**: 115200
2. **Check USB CDC configuration**:
```ini
build_flags =
    -DARDUINO_USB_CDC_ON_BOOT=1
```

## Diagnostic Tools

### **Serial Commands**
```
help                 - Show available commands
diag wifi on|off     - Control WiFi scanning
diag ble on|off      - Control BLE scanning
diag overlay on|off  - Toggle diagnostic overlay
```

### **Debug Output**
Enable various debug outputs:
- I2C operations
- Touch events
- Memory usage
- Performance metrics

### **Hardware Test Modes**
- Touch calibration mode
- Display test patterns
- I2C device scanner
- Memory stress test

## Crash Investigation Workflow

### **Automatic Reboot / Panic Analysis**

**System**: ESP32 Core Dump to Flash (256KB partition)

#### When System Reboots Unexpectedly

**Step 1: Reconnect Serial Monitor**
```bash
pio device monitor
# Baudrate: 115200
```

**Step 2: Retrieve Crash Dump**
```
crash dump
```

**Expected Output**:
```
==== ESP32 Crash Dump ====
Crash dump found!

Program Counter (PC): 0x400D1A3C
Crashed Task: UI_Task

Core Dump Version: 1
App ELF SHA256: [hash]

TROUBLESHOOTING:
1. Note the Program Counter (PC) address above
2. This indicates where in the code the crash occurred
3. Use 'crash clear' to erase this dump after investigation
4. Monitor for pattern - same PC = reproducible crash
==========================
```

**Step 3: Analyze Crash Information**

| Field | Meaning | Action |
|-------|---------|--------|
| **Program Counter (PC)** | Memory address where crash occurred | Note this value - recurring PC indicates reproducible bug |
| **Crashed Task** | FreeRTOS task that panicked | Helps narrow down which subsystem failed (UI/I2C/Network/System) |
| **Core Dump Version** | Firmware build identifier | Verify matches current build |

**Step 4: Pattern Recognition**

**Single Crash** (PC changes each time):
- Likely random hardware issue
- Power supply fluctuation
- Cosmic ray (rare but possible)
- Monitor for frequency

**Reproducible Crash** (same PC every time):
- **CRITICAL**: Indicates software bug
- Check recent code changes
- Review code at PC address (requires firmware.elf analysis)
- Add logging before crash point
- Check array bounds, null pointers, division by zero

#### Common Crash Patterns

**GPIO0 Button Not Responding Before Reboot**
- **Symptom**: Button stops working, then system reboots
- **Likely Cause**: Task watchdog timeout (UI task hung)
- **Investigation**:
  1. Check if PC is in I2C-related code
  2. Review I2C bus lockup logs
  3. Check task health: `task status`
  4. Monitor I2C failure rate: `task stats`

**Battery Monitoring Related Crashes**
- **Symptom**: Crash when battery percentage updates
- **Likely Cause**: LVGL object invalidation
- **Investigation**:
  1. Check if PC is in `updateStatusLabels()` function
  2. Verify battery label validity logs
  3. Check for "Battery label invalid" warnings in serial

**WiFi/Network Related Crashes**
- **Symptom**: Crash during WiFi scan or AP mode switch
- **Likely Cause**: WiFi mode transition race condition
- **Investigation**:
  1. Check if PC is in scanner or wifi_manager code
  2. Review WiFi mode state machine
  3. Test with WiFi disabled: `diag wifi off`

#### Crash Logging Commands

**View Last Crash**:
```
crash dump
```

**System Information**:
```
crash info
```
Shows:
- Core dump status (enabled/disabled)
- Partition size (256KB)
- Debug level (CORE_DEBUG_LEVEL=3)
- Capabilities and limitations

**Clear Crash Data**:
```
crash clear
```
Note: Crash dump will be overwritten on next panic

#### Preventive Monitoring

**Before Deploying Field Testing**:
1. Run memory stress test: `memory stress`
2. Check task health: `task status`
3. Verify I2C stability: `task stats` (check failure rate)
4. Monitor heap integrity: `memory integrity`
5. Enable battery monitoring: `battery monitor on`

**During Field Testing**:
1. Periodically check: `crash dump` (even if no obvious crash)
2. Monitor task health every hour
3. Record battery level correlation with reboots
4. Note environmental factors (temperature, vibration)

#### Advanced Debugging (Requires firmware.elf)

**Convert PC to Source Code Location**:
```bash
# Use ESP32 toolchain addr2line (not available via serial)
xtensa-esp32s3-elf-addr2line -e .pio/build/cc-radar/firmware.elf 0x400D1A3C
```

This will show exact function and line number where crash occurred.

#### Limitations

- **No full stack trace via serial**: PC address is primary diagnostic
- **Requires firmware.elf for symbol lookup**: Cannot decode PC to function name on-device
- **Single crash storage**: New panic overwrites previous dump
- **No pre-crash variable state**: Cannot inspect local variables

---

*When encountering issues, always check the serial output first for error messages and diagnostic information.*