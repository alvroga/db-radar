# GPS Modules: Beitian BH-880 (primary) and LC76G (supported alternative)

**Status**: GPS ✅ Working | Compass ✅ Working (WMM declination applied) | Dual-module support ✅ (2026-08-10)

## Module Overview

The BH-880 is a combined GNSS + magnetometer module and is the primary/recommended module — it's the
only one of the two with a compass, so it's the only one that supports heading-up navigation. It uses
the same UART pins (GPIO43/44) as the original LC76G GPS-only module it initially replaced.

**LC76G support was brought back 2026-08-10** (see [ADR-0032](adr/0032-pinned-gps-module-not-always-auto-detect.md))
so a board built with either module works from the same firmware — see "Dual-Protocol Support" below.
A board running the LC76G has no compass, so navigation is forced to North-Up automatically (there's
nothing to rotate a heading-up view by); this is a real, permanent hardware constraint of that module,
not a bug or a temporary limitation.

| Feature | Value |
|---------|-------|
| **Product** | Beitian BH-880 |
| **GPS Chip** | B1301N (NOT u-blox branded, but UBX-compatible protocol) |
| **Compass Chip** | QMC5883L (I2C address 0x0D) |
| **Default Baud** | 115200 bps (configurable 9600-2000000) |
| **Default Update Rate** | 10 Hz (100ms) |
| **Constellations** | GPS, GLONASS, BDS, Galileo, IRNSS, SBAS, QZSS (120 channels) |
| **Sensitivity** | -163 dBm tracking, -148 dBm cold start |
| **Cold Start** | 28 seconds |
| **Hot Start** | 1 second |
| **Power** | DC 3.6-5.5V, typical 5.0V, 20mA |
| **Size** | 28x28x11mm, 12g |
| **Connector** | 1.25mm 6-pin |

## Wiring

| Module Pin | Label | ESP32-S3 Pin | Function |
|-----------|-------|-------------|----------|
| 1 | D (SDA) | GPIO 15 | I2C Data (compass) |
| 2 | G (GND) | GND | Ground |
| 3 | T (TX) | GPIO 44 (ESP RX) | GPS UART TX |
| 4 | R (RX) | GPIO 43 (ESP TX) | GPS UART RX |
| 5 | V (VCC) | 5V | Power |
| 6 | C (SCL) | GPIO 7 | I2C Clock (compass) |

**Important**: The I2C bus is shared with other devices (Touch 0x15, RTC 0x51, IO Expander 0x20, IMU 0x6B). All access goes through `i2c_manager` with mutex protection.

## GPS: UBX Binary Protocol

The B1301N chip outputs **UBX binary protocol natively** (not NMEA text). The previous LC76G module used NMEA/PAIR commands which are incompatible.

### Protocol Format

```
[0xB5 0x62] [class] [id] [len_lo len_hi] [payload...] [ck_a ck_b]
```

- **Sync bytes**: 0xB5 0x62 (always)
- **Checksum**: Fletcher-8 over class + id + length + payload

### Primary Message: NAV-PVT (0x01 0x07)

92-byte payload containing position, velocity, time, and satellite info in a single message. This is the only message we parse.

| Offset | Type | Field | Conversion |
|--------|------|-------|------------|
| 4 | U2 | year | Direct |
| 6 | U1 | month | Direct |
| 7 | U1 | day | Direct |
| 8 | U1 | hour | Direct |
| 9 | U1 | minute | Direct |
| 10 | U1 | second | Direct |
| 11 | U1 | valid | Bit 0=date, Bit 1=time |
| 20 | U1 | fixType | 0=none, 2=2D, 3=3D, 4=GNSS+DR |
| 23 | U1 | numSV | Satellites in use |
| 24 | I4 | lon | * 1e-7 -> degrees |
| 28 | I4 | lat | * 1e-7 -> degrees |
| 36 | I4 | hMSL | mm -> meters (* 0.001) |
| 60 | I4 | gSpeed | mm/s -> knots (* 0.00194384) |
| 64 | I4 | headMot | * 1e-5 -> degrees |
| 76 | U2 | pDOP | * 0.01 -> HDOP |

### UBX ACK Protocol

Every configuration command receives a response:

| Response | Class | ID | Meaning |
|----------|-------|----|---------|
| ACK-ACK | 0x05 | **0x01** | Command accepted |
| ACK-NAK | 0x05 | **0x02** | Command rejected |

`waitForAck(cls, id)` waits up to 500ms for one of these. Baud rate change is the only command that skips ACK — the module switches baud before it can reply.

### Messages We Parse

| Message | Class | ID | Payload | Used for |
|---------|-------|----|---------|----------|
| **NAV-PVT** | **0x01** | **0x07** | 92 bytes | Everything — position, time, speed, heading |
| MON-VER | 0x0A | 0x04 | variable | `gps info` serial command only |

NAV-PVT is the only message in the main read loop. The struct also includes a `magDec` field (offset 88, deg×0.01) — the chip computes its own magnetic declination — but we use WMM instead for accuracy and transparency. See [`docs/wmm_declination.md`](wmm_declination.md).

### Configuration Commands Used

| Command | Class/ID | Notes |
|---------|----------|-------|
| Enable NAV-PVT | 0x06/0x01 | Sent every boot in `begin()` — payload `{0x01, 0x07, 0x01}` |
| Set Update Rate | 0x06/0x08 (CFG-RATE) | `measRate` in ms, ACK confirmed |
| Set Baudrate | 0x06/0x00 (CFG-PRT) | 20-byte UART config, no ACK wait |
| Save Config | 0x06/0x09 (CFG-CFG) | `saveMask=0x1F`, ACK confirmed |
| Factory Reset | 0x06/0x09 (CFG-CFG) | `clearMask+loadMask=0x1F`, ACK confirmed |
| Hot Start | 0x06/0x04 (CFG-RST) | `navBbrMask=0x0000, resetMode=0x02` — ⚠️ broken, see below |
| Warm Start | 0x06/0x04 (CFG-RST) | `navBbrMask=0x0001, resetMode=0x02` — ⚠️ broken, see below |
| Cold Start | 0x06/0x04 (CFG-RST) | `navBbrMask=0xFFFF, resetMode=0x02` — ⚠️ broken, see below |

**⚠️ Restart commands not working**: Hot/warm/cold restart via UBX-CFG-RST always ACK-timeout. The B1301N resets before it can send the ACK. `resetMode=0x02` (GPS-only reset) may not be supported identically to u-blox. Workaround: physically unplug to cold start. Potential fix: try `resetMode=0x01` (controlled software reset).

### Serial Commands

```
gps status              - Show GPS fix status and coordinates
gps quality             - Detailed quality report
gps raw [seconds]       - Dump raw UBX hex data (default 5s, max 30s)
gps config rate <ms>    - Set update rate (25-10000ms, default 100)
gps config baud <rate>  - Change baudrate (9600-921600)
gps restart hot|warm|cold - Restart GPS module
gps reset               - Factory reset (clears all settings)
```

### Key Code Files

| File | Purpose |
|------|---------|
| `src/hardware/sensors/gps_bh880.cpp` | UBX state machine parser, config commands |
| `include/hardware/sensors/gps_bh880.h` | GPSData struct, public API |
| `src/core/device_manager.cpp` | GPS initialization during boot |
| `src/utils/diagnostics.cpp` | GPS serial commands |

### Differences from LC76G

| Feature | LC76G | BH-880 |
|---------|-------|--------|
| Protocol | NMEA text + PAIR commands | UBX binary |
| Baud detect | Look for `$` character | Look for 0xB5 0x62 sync |
| Config commands | PAIR004/005/006/007/513/050/864 | UBX-CFG-RATE, CFG-RST, etc. |
| Default rate | 1 Hz | 10 Hz |
| GNSS systems | Configurable via PAIR066 (not exposed — see below) | All enabled by default |
| Compass | None — North-Up only | QMC5883L built-in |

`gps_bh880.cpp` now speaks both — see "Dual-Protocol Support" below for how the right one gets picked.
`setGNSSSystems()`/PAIR066 from the pre-BH-880 LC76G driver was **not** ported back; every other PAIR
command (`hotStart`/`warmStart`/`coldStart`/`factoryReset`/`saveConfig`/`setUpdateRate`/`setBaudrate`)
was.

---

## Dual-Protocol Support: BH-880 + LC76G

One firmware image supports either module, auto-identified from the byte stream at first boot and
then pinned as a persisted choice — not re-detected on every boot. Full design rationale, alternatives
considered, and why the detection had to be two strict sequential passes rather than one interleaved
pass: [ADR-0032](adr/0032-pinned-gps-module-not-always-auto-detect.md).

### How module identification works

`gps_bh880::detectBaud()` runs **two sequential passes**, each cycling the same 6 candidate baud rates
(115200, 9600, 38400, 57600, 230400, 460800):

1. **UBX pass** (`detectBaudUBX()`) — byte-identical to the original BH-880-only algorithm: look for
   the `0xB5 0x62` sync pair repeating 3 times. If found on any rate, done — a BH-880 always resolves
   here, at the same speed it always has.
2. **NMEA pass** (`detectBaudNMEA()`) — only reached if pass 1 finds nothing on all 6 rates. Looks for
   2 complete, checksum-valid NMEA sentences (`$...*CC\r\n`).

Once a session confirms a protocol (either pass, or the first successfully-parsed live frame),
`read()`'s byte parser locks to it — a `'$'` byte is thereafter ignored exactly like any other
non-sync byte, the same as the original UBX-only parser always did. This is what makes a BH-880 that
also happens to emit default NMEA chatter alongside UBX immune to ever being misclassified mid-session.

### Module pinning (Settings > GPS)

Detection above only ever runs on a board that hasn't been configured yet. `settings_manager` persists
`gps_module_type` (0=BH-880, 1=LC76G) and `gps_module_configured` (NVS, survives both the OTA-only and
full-flash web-flasher images — see [`docs/firmware_installation.md`](firmware_installation.md) for why
NVS's `0x9000-0xd000` region is never touched by either). Once pinned, `device_manager::initGPS()`
calls `gps_bh880::beginWithProtocol()` instead of `begin()` — skips protocol detection entirely and
runs only the single relevant pass (`detectBaudUBX()` or `detectBaudNMEA()` directly) for baud, since
baud can still vary per unit/config even when the module type is known.

**First boot**: before `gps_module_configured` is ever true, a one-time full-screen picker appears
(`main.cpp`, right after `ui_manager::init()`, before the loading-screen sequence) showing what the
auto-detect scan found this boot as a hint, but always requiring an explicit tap. GPS itself isn't
gated by this picker — it already came up via the two-pass auto-detect earlier in boot (before display
was even ready); the picker only decides what *future* boots do.

**Changing the pin later**: Settings > GPS has a module dropdown + "Save + Reboot to Apply" button
(same `esp_restart()` pattern as the WiFi/AP screens). The tab's old Hot/Warm/Cold Start and Factory
Reset touch buttons were removed in the same change — the web flasher gives full reflash control now,
so those are redundant UI weight. The underlying commands are unaffected and still reachable via
`gps restart hot|warm|cold` and `gps reset` on serial (see Serial Commands above; PAIR-equivalent
branches were added to each so they work correctly against an LC76G too, not just a BH-880).

### No compass on LC76G — North-Up is forced automatically

`device_manager`'s `compass_ok` flag (already false-safe: `initCompass()` probes the I2C address
before ever attempting a real register read, so a genuinely absent chip fails cleanly with no bus
errors or retries) now also drives navigation mode. If `compass_ok` is false at `ui_manager::init()`,
`heading_up_mode` is forced off in RAM (not written to NVS — a saved Heading-Up preference from a
BH-880 session is preserved and reapplied if the compass is ever present again), and the Settings
nav-mode dropdown is locked to North-Up (`LV_STATE_DISABLED`) instead of offering a Heading-Up option
that would silently do nothing.

### Key files added/changed for dual-protocol support

| File | Role |
|------|------|
| `src/hardware/sensors/gps_bh880.cpp` | NMEA/PAIR parser ported in alongside UBX; two-pass `detectBaud()`; `beginWithProtocol()` |
| `include/hardware/sensors/gps_bh880.h` | `GpsModule` enum, `beginWithProtocol()`, `isNmeaProtocol()`/`protocolName()` |
| `src/core/device_manager.cpp` | `initGPS()` branches on `gps_module_configured` |
| `src/utils/settings_manager.cpp` / `.h` | `gps_module_type`/`gps_module_configured`, `saveGPSModuleSelection()` |
| `src/core/main.cpp` | First-boot picker (`showGpsModulePickerBlocking()`) |
| `src/ui/ui_manager.cpp` | Forces North-Up when `!compass_ok` |
| `src/ui/settings_screen.cpp` | GPS Module dropdown + reboot button; nav-mode dropdown disabled when no compass; Restart/Factory-Reset buttons removed |

---

## Compass: QMC5883L Magnetometer

The BH-880 includes a QMC5883L 3-axis magnetometer connected via I2C.

### Hardware Details

| Feature | Value |
|---------|-------|
| **Chip** | QMC5883L |
| **I2C Address** | 0x0D |
| **I2C Bus** | Shared (SDA=15, SCL=7, 100kHz) |
| **Chip ID Register** | 0x0D, expected value: 0xFF |
| **Output** | 16-bit signed X, Y, Z (magnetic field) |
| **Field Range** | +/-2 Gauss or +/-8 Gauss (configurable) |
| **ODR** | 10, 50, 100, or 200 Hz |
| **Resolution** | 2 mGauss per LSB (2G range) |

### Register Map

| Register | Address | Purpose |
|----------|---------|---------|
| DATA_X_LSB | 0x00 | X-axis data low byte |
| DATA_X_MSB | 0x01 | X-axis data high byte |
| DATA_Y_LSB | 0x02 | Y-axis data low byte |
| DATA_Y_MSB | 0x03 | Y-axis data high byte |
| DATA_Z_LSB | 0x04 | Z-axis data low byte |
| DATA_Z_MSB | 0x05 | Z-axis data high byte |
| STATUS | 0x06 | Bit 0=DRDY, Bit 1=OVL, Bit 2=DOR |
| TEMP_LSB | 0x07 | Temperature low byte |
| TEMP_MSB | 0x08 | Temperature high byte |
| CONTROL1 | 0x09 | Mode, ODR, Range, OSR |
| CONTROL2 | 0x0A | Soft reset, pointer rollover |
| SET_RESET | 0x0B | SET/RESET period (write 0x01) |
| CHIP_ID | 0x0D | Chip identification (reads 0xFF) |

### Control Register 1 (0x09)

```
Bits 7-6: OSR (Over-Sampling Ratio)
  00 = 512 (best noise, slowest)
  01 = 256
  10 = 128
  11 = 64  (fastest)

Bits 5-4: RNG (Full Scale Range)
  00 = 2 Gauss (higher resolution)
  01 = 8 Gauss (higher range, for strong fields)

Bits 3-2: ODR (Output Data Rate)
  00 = 10 Hz
  01 = 50 Hz
  10 = 100 Hz
  11 = 200 Hz

Bits 1-0: MODE
  00 = Standby
  01 = Continuous measurement
```

### Initialization Sequence

1. Write 0x01 to SET_RESET register (0x0B) - recommended by datasheet
2. Write config to CONTROL1 (0x09):
   - Continuous mode (0x01)
   - 200 Hz ODR (0x0C)
   - 2G range (0x00)
   - 512 OSR (0x00)
   - Combined: `0x01 | 0x0C | 0x00 | 0x00` = **0x0D**
3. Read CHIP_ID (0x0D) to verify - should return 0xFF

### Computing Heading

```cpp
// Read 6 bytes starting at register 0x00
int16_t x = (msb_x << 8) | lsb_x;
int16_t y = (msb_y << 8) | lsb_y;
// int16_t z = (msb_z << 8) | lsb_z;  // Not needed for 2D heading

// Calculate heading (degrees from magnetic north)
float heading = atan2(y, x) * 180.0f / M_PI;
if (heading < 0) heading += 360.0f;
```

**Important Notes**:
- This raw formula gives **magnetic heading**, not true heading, and has no calibration
  applied — it's shown here for the underlying register math, not as what the firmware
  actually computes today.
- The shipped implementation is considerably more complete: hard-iron calibration,
  health classification, magnetic-declination correction (WMM), and accelerometer-based
  tilt compensation are all built and field-verified — see below.

### Serial Commands

```
compass status          - Read chip ID and show compass status
compass read            - Read and display raw X/Y/Z values and computed heading
compass stream [s]      - Stream compass readings for N seconds (default 5)
```

### Calibration and Tilt Compensation

Hard-iron calibration, health classification, magnetic declination (WMM), and
accelerometer-based tilt compensation are all implemented and shipped — not future work.
Full detail (formulas, EMA time constants, calibration procedure, field-verification
history): [`docs/compass.md`](compass.md).

### Key Code Files

| File | Purpose |
|------|---------|
| `include/hardware/i2c/i2c_manager.h` | COMPASS_DEVICE handle (0x0D) |
| `src/utils/diagnostics.cpp` | `compass` serial commands for testing |

---

## I2C Bus Devices (Complete)

All devices on the shared I2C bus (SDA=15, SCL=7 @ 100kHz):

| Address | Device | Source |
|---------|--------|--------|
| 0x0D | QMC5883L (Compass) | BH-880 module |
| 0x15 | CST820 (Touch) | Waveshare board |
| 0x20 | TCA9554 (IO Expander) | Waveshare board |
| 0x51 | PCF85063 (RTC) | Waveshare board |
| 0x6A | QMI8658 (IMU, low address) | Waveshare board |
| 0x6B | QMI8658 (IMU, high address) | Waveshare board |

A scan will sometimes also show `0x7E` — that's not a real device, it's a probe artifact
at a reserved I2C address that survives even a double-ACK confirmation guard. See
[`docs/i2c_bus_freeze_investigation.md`](i2c_bus_freeze_investigation.md) for detail.

---

**Compass software implementation**: See [`docs/compass.md`](compass.md) for heading pipeline, calibration, WMM declination, I2C constraints, and upgrade path.

*Last updated: 2026-08-10 (dual-protocol LC76G support added — see "Dual-Protocol Support" above and
[ADR-0032](adr/0032-pinned-gps-module-not-always-auto-detect.md))*
