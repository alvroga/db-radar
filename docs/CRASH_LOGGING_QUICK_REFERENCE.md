# Crash Logging Quick Reference Guide

**Quick access guide for troubleshooting automatic reboots and system panics**

---

## 🚨 When Device Reboots Unexpectedly

### Step 1: Reconnect Serial Monitor
```bash
pio device monitor
# Baudrate: 115200
```

### Step 2: Check for Crash Dump
```
crash dump
```

### Step 3: Interpret Results

#### If "Crash dump found!"
```
==== ESP32 Crash Dump ====
Crash dump found!

Program Counter (PC): 0x400D1A3C    ← Note this address!
Crashed Task: UI_Task               ← Which task panicked

Core Dump Version: 1
App ELF SHA256: [hash]
==========================
```

**Action Required**:
1. **Write down the PC address** (e.g., 0x400D1A3C)
2. **Note the crashed task** (UI/I2C/Network/System)
3. **Monitor for pattern** - If same PC repeats → reproducible bug!

#### If "No crash dump found"
```
No crash dump found
This is normal if no panic has occurred since last clear
```

**This is GOOD** - No crash detected. Reboot was clean shutdown.

---

## 📋 Common Crash Patterns

### Pattern 1: Same PC Address Every Time
**Example**: Crash always shows `PC: 0x400D1A3C`

**Meaning**: **CRITICAL** - Reproducible software bug

**Action**:
1. Check recent code changes
2. Add logging before crash point
3. Review code for:
   - Array bounds violations
   - Null pointer dereferences
   - Division by zero
   - LVGL object use-after-free

### Pattern 2: Different PC Each Time
**Example**: First crash `PC: 0x400D1A3C`, second crash `PC: 0x400E52F8`

**Meaning**: Random hardware issue or environment factor

**Possible Causes**:
- Power supply fluctuation
- Battery voltage dip
- Environmental interference
- Temperature extremes
- Cosmic ray (rare but possible)

**Action**: Monitor frequency and environmental correlation

### Pattern 3: Specific Task Always Crashes
**Example**: Always shows `Crashed Task: I2C_Task`

**Meaning**: Issue in I2C subsystem (bus lockup, timeout, device failure)

**Action**:
```bash
task status    # Check I2C task health
task stats     # Check I2C failure rate
```

---

## 🔍 Crash Investigation by Task

### UI_Task Crashes
**Common Causes**:
- LVGL object invalidation
- Touch event handling errors
- Button state machine issues
- Screen transition bugs

**Investigation**:
```bash
memory stats      # Check heap/PSRAM usage
memory integrity  # Verify heap not corrupted
```

### I2C_Task Crashes
**Common Causes**:
- I2C bus lockup
- Device timeout
- Queue overflow
- Mutex deadlock

**Investigation**:
```bash
task stats        # Check I2C request failure rate
diag wifi off     # Disable WiFi to reduce bus contention
```

### Network_Task Crashes
**Common Causes**:
- WiFi mode transition race
- BLE scan timeout
- Network stack overflow

**Investigation**:
```bash
diag wifi off     # Disable WiFi scanning
diag ble off      # Disable BLE scanning
```

### System_Task Crashes
**Common Causes**:
- Memory corruption
- Watchdog timeout
- Stack overflow

**Investigation**:
```bash
memory stress     # Run stress test
memory leak start # Enable leak detection
# Wait 5 minutes
memory leak report
```

---

## 🛠️ Diagnostic Commands

### View Last Crash
```
crash dump
```
Shows: PC address, crashed task, core dump version

### System Information
```
crash info
```
Shows: Capabilities, usage instructions, limitations

### Clear Crash Data
```
crash clear
```
Note: Crash dump will be overwritten on next panic anyway

---

## ⚠️ Troubleshooting Garbled Output

### Problem: `crash dump` Shows Garbled Data

**Symptoms**:
- "Bunch of numbers and zeros" flooding serial monitor
- Unreadable output
- Serial monitor becomes unresponsive

**Causes**:
- Serial buffer overflow (core dump data too large)
- USB CDC timing issues
- Corrupted flash partition

**Solutions**:

#### Solution 1: Use Boot Messages Instead (RECOMMENDED)
Boot messages are more reliable than the `crash dump` command!

1. Keep serial monitor **open BEFORE crash happens**
2. When device reboots, look for "Guru Meditation Error"
3. Find PC address in boot output (see below)

#### Solution 2: Increase Serial Buffer
```bash
# Close current monitor
# Reopen with raw mode
pio device monitor --raw
```

#### Solution 3: Verify System First
```bash
crash info    # Check if crash logging is configured correctly
```

---

## 📟 Reading ESP32 Boot Messages (BEST METHOD)

### When Device Reboots - Look at Boot Output

**After any reboot, ESP32 prints detailed panic info to serial:**

```
ets Jun  8 2016 00:22:57

rst:0x10 (RTCWDT_RTC_RESET),boot:0x13 (SPI_FAST_FLASH_BOOT)
configsip: 0, SPIWP:0xee
...

Guru Meditation Error: Core  1 panic'ed (LoadProhibited). Exception was unhandled.

Core  1 register dump:
PC      : 0x400d1a3c  PS      : 0x00060330  A0      : 0x800d1b50  A1      : 0x3ffb1234
                     ^^^^^^^^ THIS IS THE IMPORTANT PART!
SP      : 0x3ffb1234  A2      : 0x3ffb2000  A3      : 0x00000000  A4      : 0x00000001
...

Backtrace:
0x400d1a3c:0x3ffb1234 0x800d1b50:0x3ffb1250 0x400d2c14:0x3ffb1270
```

**What to capture**:
1. **PC address**: `0x400d1a3c` ← Most important!
2. **Exception type**: `LoadProhibited` ← What went wrong
3. **Core number**: `Core 1` ← Which CPU core crashed

### Exception Types Explained

| Exception | Meaning | Common Cause |
|-----------|---------|--------------|
| **LoadProhibited** | Tried to read from invalid/protected memory | Null pointer dereference, reading freed memory, bad array access |
| **StoreProhibited** | Tried to write to invalid/protected memory | Writing to freed memory, LVGL object after delete, const violation |
| **IllegalInstruction** | CPU tried to execute invalid instruction | Stack corruption, jumped to wrong address, corrupted function pointer |
| **InstrFetchProhibited** | Tried to fetch code from invalid address | Function pointer corruption, return address corrupted |
| **IntegerDivideByZero** | Division by zero | Math error, uninitialized variable used as divisor |
| **Interrupt** | Unhandled interrupt exception | Hardware interrupt not properly handled |
| **DoubleException** | Exception occurred while handling exception | Critical - usually stack overflow |

### Example Boot Message Analysis

```
Guru Meditation Error: Core 1 panic'ed (LoadProhibited)
PC: 0x400d1a3c
```

**This tells you**:
- UI Task likely crashed (Core 1 runs UI)
- Tried to READ from bad memory address
- PC `0x400d1a3c` is where it happened
- Use addr2line to find exact source line

---

## 🎯 Recommended Workflow

### Best Practice: Use Boot Messages, Not `crash dump`

**Why?**
- ✅ More reliable (printed directly by ESP32 ROM)
- ✅ Shows exception type (LoadProhibited, etc.)
- ✅ Shows register dump
- ✅ Shows backtrace (call stack)
- ✅ No serial buffer issues

**How?**
1. Keep `pio device monitor` running during testing
2. When reboot happens, scroll up in serial output
3. Look for "Guru Meditation Error"
4. Note PC address and exception type
5. Use `addr2line` to find source location (see Advanced Analysis)

**When to use `crash dump`?**
- Only if you missed boot messages
- As confirmation of PC address
- To check if crash was saved to flash

---

## 📊 Preventive Monitoring

### Before Field Testing
```bash
# Run all health checks
memory stress
task status
memory integrity
battery monitor on
```

### During Field Testing (Every Hour)
```bash
# Quick health check
crash dump         # Check for any crashes
task status        # Verify all tasks healthy
battery status     # Monitor battery correlation
```

### After Field Testing
```bash
# Generate comprehensive report
crash dump
task stats
memory report
battery status
```

---

## 🔬 Advanced Analysis (Requires PC/Mac)

### Convert PC Address to Source Code

**Requirement**: PlatformIO toolchain installed

**Command**:
```bash
# Replace 0x400D1A3C with actual PC address from crash dump
xtensa-esp32s3-elf-addr2line -e .pio/build/cc-radar/firmware.elf 0x400D1A3C
```

**Example Output**:
```
src/ui/navigation.cpp:425
```

This shows **exact line** where crash occurred!

---

## 🚦 Crash Severity Guide

| PC Pattern | Task | Frequency | Severity | Action |
|------------|------|-----------|----------|--------|
| Same PC | Any | Every reboot | 🔴 CRITICAL | Fix immediately - reproducible bug |
| Same PC | UI_Task | Occasional | 🟡 HIGH | Review LVGL object lifecycle |
| Same PC | I2C_Task | Occasional | 🟡 HIGH | Check I2C bus stability |
| Different PC | Any | Rare (<1/day) | 🟢 LOW | Monitor pattern, environmental |
| Different PC | Any | Frequent (>5/day) | 🟠 MEDIUM | Check power supply, battery |

---

## 📝 Crash Log Template (For Reporting)

```
==== CRASH REPORT ====
Date: YYYY-MM-DD HH:MM
Session Duration: X hours
Battery Level: XX%
Environment: Indoor/Outdoor, Temp XX°C

Crash Info:
- PC: 0xXXXXXXXX
- Task: [UI/I2C/Network/System]
- Occurrence: [First time / Nth time]
- Pattern: [Same PC / Different PC]

What I Was Doing:
- [Describe activity before crash]

Environmental Factors:
- [Temperature, vibration, power events, etc.]

Task Health (before crash):
- [Output of 'task status' if available]

I2C Stats (before crash):
- [Output of 'task stats' if available]

Additional Notes:
- [Any other observations]
======================
```

---

## ✅ Quick Checklist After Unexpected Reboot

- [ ] Reconnect serial monitor
- [ ] Type `crash dump` to check for panic
- [ ] Note PC address (if crash found)
- [ ] Note crashed task name
- [ ] Compare with previous crashes (pattern?)
- [ ] Check battery level correlation
- [ ] Run `task status` to verify system health
- [ ] Run `memory integrity` to check heap
- [ ] Document in crash log template
- [ ] Continue monitoring for pattern

---

## 🆘 Need Help?

**If you see repeated crashes with same PC**:
1. Note the PC address
2. Run addr2line to get source location (see Advanced Analysis)
3. Review code at that location
4. Add Serial.printf() debugging before crash point
5. Check GitHub issues for similar patterns
6. Consider posting crash dump to development team

**If crashes are random (different PC each time)**:
1. Monitor environmental factors (temperature, vibration)
2. Check battery voltage during crashes
3. Test with external power supply
4. Run memory stress test: `memory stress`
5. Monitor I2C bus health: `task stats`

---

**System Status**: ✅ Crash logging active (256KB partition)
**Debug Level**: CORE_DEBUG_LEVEL=3
**Persistence**: Survives reboot
**Access**: Serial commands (115200 baud)

---

*Last Updated*: 2025-10-18
*Feature Version*: 1.0.0
*Platform*: ESP32-S3 (Waveshare Touch LCD 2.1)
