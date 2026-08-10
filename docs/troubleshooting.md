# Troubleshooting Guide

## Common Issues and Solutions

### **Boot hangs immediately after `[I2C] Initialized: SDA=15, SCL=7...`**

**Symptoms**: Boot log stops dead on that line. Nothing follows — no
`[DEVICE] Initializing IO Expander (EXIO)...`. Pressing reset does not help. Reflashing does not help.

**Cause**: a **stuck I2C bus**. The statement immediately after that print is `ping(EXIO_DEVICE)`
(`i2c_manager.cpp`), the first real transaction on the bus. If the MCU resets while a slave — the
CST820 touch controller or the PCF85063 RTC — is mid-byte, that slave keeps holding SDA low waiting to
finish a transfer that will never come. **An MCU reset does not reset the slaves**; they are separate
chips with their own power, so the bus comes up already jammed.

Most likely after many rapid reset/reflash cycles, which is exactly when you are least inclined to
suspect the hardware and most inclined to suspect your last commit.

**Fix**: **full power cycle.** Unplug USB *and* disconnect the battery for ~10 s so the slaves lose
power and release the lines. The reset button is not enough — that is the entire point of the failure
mode. Confirmed to resolve it (2026-07-31).

**Diagnostic value**: if a power cycle fixes it, it was the bus, not your code. A code fault would
reproduce deterministically across power cycles; this does not.

**Full detail**: [`docs/i2c.md`](i2c.md) and [`docs/i2c_bus_freeze_investigation.md`](i2c_bus_freeze_investigation.md).

### **Display Issues**

#### Display Jitter/Flicker
**Symptoms**: Screen flickering or unstable display
**Causes**:
- Incorrect timing parameters
- PCLK frequency too high
- Poor power supply

**Solutions**: this project's timing baseline is already tuned (10MHz PCLK, specific porch values) —
see [`docs/display.md`](display.md) for the full RGB timing table and why each value is what it is,
rather than guessing at new numbers.

#### No Display Output
**Symptoms**: Blank screen, backlight may work
**Causes**:
- LCD_CS not properly controlled
- SPI initialization failed
- RGB timing misconfiguration

**Solutions**:
1. Verify LCD_CS is held HIGH after init
2. Check SPI command sequence
3. Monitor serial output for timing confirmation — see [`docs/display.md`](display.md)

#### Color Issues
**Symptoms**: Wrong colors, color bleeding
**Causes**:
- Incorrect RGB pin mapping
- Signal integrity issues

**Solutions**: verify the `DATA_PINS` array matches CLAUDE.md's GPIO Pin Assignments table exactly —
a single swapped pin here shows up as wrong colors, not a blank screen.

### **I2C Communication Errors**

This project uses a unified I2C manager (`i2c_manager.cpp`/`.h`, ESP-IDF's `driver/i2c_master.h`) —
there is no `Wire` library anywhere in this codebase, so any advice referencing `Wire.begin()` /
`Wire.beginTransmission()` is for a different (Arduino-framework) build and does not apply here.

#### Frequent Bus Errors / Device Not Responding
**Symptoms**: repeated I2C failures logged for a specific device, or a device that never responds.

**Diagnose and fix via**:
- `i2c_manager` already retries and tracks per-device failure stats — check `config show` or the
  relevant serial diagnostics for current bus health rather than adding new throttling code.
- Verify device addresses against the table in [`docs/i2c.md`](i2c.md): Touch (CST820, 0x15), RTC
  (PCF85063, 0x51), IO Expander (TCA9554, 0x20), Compass (QMC5883L, 0x0D), IMU (QMI8658, 0x6A/0x6B).
- If the symptom is a full boot hang rather than intermittent errors, see the stuck-bus section above
  — that is a different failure mode with a different fix (power cycle, not a code change).

**Full guide** (retry logic, bus recovery, historical EXIO pin-mapping bug):
[`docs/i2c.md`](i2c.md).

### **Touch Issues**

**Symptoms**: no touch events, touch offset/inaccurate coordinates, touch works but at the wrong
position.

The touch coordinate pipeline (CST820 register reads, coordinate scaling, circular-display masking)
is documented in detail, with exact current code references, in [`docs/touch.md`](touch.md) — consult
that rather than a simplified snippet here, since the scaling/inversion logic is more involved than a
one-line swap and has changed over the project's history.

### **WiFi/BLE Issues**

#### WiFi Scanning Shows 0 Networks
**Symptoms**: WiFi count always 0

**Solutions**:
1. Confirm WiFi scanning is actually enabled: `diag wifi on`
2. Check `wifi_manager`'s connection state and mode — see
   [`docs/wifi_implementation_guide.md`](wifi_implementation_guide.md) for the full boot-mode
   architecture (AP standalone / STA standalone / in-session) and which mode you're actually in.
3. WiFi and BLE radios are mutually exclusive on this project — NimBLE only initializes when WiFi is
   not active. If BLE is active, WiFi scanning will show nothing by design, not by fault.

#### BLE Scanning Not Working
**Symptoms**: BLE count always 0, or a configured beacon is never found

**This project uses NimBLE, not the standard `BLEDevice.h` stack — that migration (ADR-0009) is
deliberate and saved ~40KB SRAM. Do not "fix" this by switching back to standard BLE.**

**Solutions**:
1. Passive scanning is load-bearing here, not a power-saving choice — active scanning defers the
   `onResult` callback until a scan response arrives, which a legacy `ADV_IND` advertiser may never
   send. Don't change `setActiveScan()` without reading why first.
2. If a specific target device is never found: `beacon status` disambiguates "the scan itself is
   broken" (near-zero scan callbacks) from "the scan works but never sees this device" (many
   callbacks, zero matching the target MAC) — the two look identical from the outside otherwise.
3. The target device must advertise in **Legacy (BLE 4.0)** mode — BLE 5.0 Extended
   Advertising/Coded PHY is invisible to this scanner and looks exactly like a dead/out-of-range
   device.

**Full guide**: [`docs/beacon_proximity.md`](beacon_proximity.md).

### **Memory Issues**

There is no `ESP.getFreeHeap()` / `ESP.getFreePsram()` in this codebase (those are Arduino-framework
calls) — this is an ESP-IDF build. Use the `memory` serial command family instead:

```
memory stats       Current heap/PSRAM/LVGL usage
memory integrity   Check heap integrity
memory report      Full system memory report
memory leak start|stop|report   Leak detection over a time window
```

**Full guide** (pool sizes, corruption checks, the reasoning behind the current conservative
allocation): [`docs/memory_management.md`](memory_management.md).

#### Stack Overflow
**Symptoms**: core panic, stack-related crash
**Causes**: large local variables, deep recursion, insufficient task stack size

**Solutions**: task stack sizes are defined in `include/utils/task_manager.h`
(`TaskConfig::UI_STACK_SIZE` etc.) — check `task status` for current stack high-water marks before
assuming a size increase is needed.

### **Performance Issues**

#### Low Frame Rate
**Symptoms**: sluggish UI, low FPS

The render pipeline is a zero-copy design with several load-bearing constraints (clip_corner off,
`radar_obj` not clickable, `full_refresh` tied to rotation mode) — read CLAUDE.md's Render Pipeline
section **before** changing anything here, since several of these flags look like reasonable
optimizations but actually reintroduce measured regressions if touched. Use the `perf` serial command
to see current frame timing broken down by stage rather than guessing at the bottleneck.

### **Development Issues**

#### Upload Failures
**Symptoms**: cannot upload firmware
**Solutions**:
1. Reduce upload speed: 921600 → 460800 → 115200
2. Try a different USB cable — charge-only cables have no data lines
3. Hold GPIO0 during reset if the board doesn't enter bootloader mode automatically

#### Local build fails with host-toolchain errors (`-arch arm64`, `-mlongcalls` unknown, missing `src.c`, etc.)
**Symptoms**: `pio run` fails inside a CMake `TryCompile` step, with errors that make no sense for an
Xtensa cross-build — e.g. `unrecognized command-line option '-arch'`, `cc: error: unknown argument
'-mlongcalls'; did you mean '-mlong-calls'?`, or `Cannot find source file: .../TryCompile-XXXXXX/src.c`.
Look closely and the compiler path in the error is the **host** Apple clang
(`/Library/Developer/CommandLineTools/usr/bin/cc`), not
`toolchain-xtensa-esp-elf/bin/xtensa-esp32s3-elf-gcc` — CMake's cached toolchain detection got
confused, not a real code problem. Seen after switching the `platform` version pin in
`platformio.ini` and after several back-to-back builds in the same session; root cause not fully
pinned down, but it's consistently a stale `.pio/build` cache, never the source.

**Solution**: `rm -rf .pio/build` (the whole directory, not just the env subfolder) and rebuild.
`pio run -t clean` alone has not reliably fixed this — go straight to the manual `rm -rf`.

#### Serial Monitor Not Working
**Symptoms**: no serial output, or output stops mid-session

**Solutions**:
1. Verify baud rate: 115200
2. USB CDC is configured via `sdkconfig.defaults` on this build, not a `platformio.ini` build flag —
   there is no `-DARDUINO_USB_CDC_ON_BOOT` flag to check here (that was pre-migration Arduino
   configuration).
3. If output stopped because logging was explicitly disabled: `serial on` re-enables it — the
   `serial off`/`serial on` gate itself still responds even while logging is off.

## Diagnostic Tools

### **Serial Commands**
```
help                    Show all available commands
diag wifi on|off        Enable/disable WiFi scanning
diag ble on|off         Enable/disable BLE scanning
diag freeze on|off      Freeze/unfreeze LVGL display (testing)
memory stats|report|... Memory diagnostics — see Memory Issues above
task status             FreeRTOS task health and statistics
config show|display|timing|pins   Current configuration values
battery status|voltage|percent|raw|info|monitor on|off
beacon status|scan|test|zone|trend
perf                    Frame timing breakdown by render stage
```

## Crash Investigation Workflow

**Core dump to flash is currently disabled in this build** (`CONFIG_ESP_COREDUMP_ENABLE_TO_NONE=y`
in `sdkconfig.db-radar`) — `crash dump` will correctly report nothing found, even after a real crash.
Until that's re-enabled, the reliable method is reading the boot output directly.

### Recommended: read the boot message, not `crash dump`

**Keep `pio device monitor` running before a crash happens** — when the device reboots after a
panic, ESP-IDF prints a full register dump and backtrace directly to serial:

```
Guru Meditation Error: Core  1 panic'ed (LoadProhibited). Exception was unhandled.

Core  1 register dump:
PC      : 0x400d1a3c  PS      : 0x00060330  A0      : 0x800d1b50  A1      : 0x3ffb1234
                     ^^^^^^^^ THIS IS THE IMPORTANT PART!
...
Backtrace:
0x400d1a3c:0x3ffb1234 0x800d1b50:0x3ffb1250 0x400d2c14:0x3ffb1270
```

**What to capture**: the PC address (`0x400d1a3c`), the exception type (`LoadProhibited`), and which
core panicked (Core 1 runs the UI Task; Core 0 runs I2C/Network/System).

| Exception | Meaning | Common Cause |
|-----------|---------|---------------|
| **LoadProhibited** | Read from invalid/protected memory | Null pointer dereference, use-after-free, bad array access |
| **StoreProhibited** | Write to invalid/protected memory | Writing to freed memory, LVGL object after delete |
| **IllegalInstruction** | CPU tried to execute an invalid instruction | Stack corruption, corrupted function pointer |
| **InstrFetchProhibited** | Tried to fetch code from an invalid address | Function pointer / return address corruption |
| **IntegerDivideByZero** | Division by zero | Uninitialized variable used as a divisor |
| **DoubleException** | Exception occurred while handling another exception | Usually stack overflow |

**Converting the PC address to a source location** (requires the matching `firmware.elf`):
```bash
xtensa-esp32s3-elf-addr2line -e .pio/build/db-radar/firmware.elf 0x400D1A3C
```

### If the same PC recurs across multiple crashes
That's a reproducible software bug, not environmental noise — check recent changes, and review the
code at that address for array bounds, null pointers, or LVGL object lifetime issues (see CLAUDE.md's
LVGL Screen Lifecycle notes — this is a common source of exactly this crash class).

### If the PC differs every time
More consistent with a hardware/environmental cause (power supply dip, battery voltage sag under
load) than a code defect. Correlate with `battery status` and check for patterns in when it happens
(charging transitions, brownout-adjacent voltage, temperature extremes).

### Before field testing
```bash
memory stress       # Run stress test
task status          # Verify all tasks healthy
memory integrity     # Verify heap not corrupted
battery monitor on   # Enable periodic battery logging for correlation
```

---

*When encountering issues, always check the serial output first for error messages and diagnostic
information. If a section above conflicts with what you observe in current code, trust the code and
the linked component doc — this file is a triage index, not the source of truth for any subsystem.*
