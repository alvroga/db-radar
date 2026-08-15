# GPS Modules: Beitian BH-880 (primary), LC76G, BN-880, and BE-881

**Status**: GPS ✅ Working | Compass ✅ Working (WMM declination applied) | Four modules supported in firmware, **three qualified** for the first-boot picker (BH-880, LC76G, BE-881 — all field-verified); BN-880 build-verified only, kept in the firmware, reachable via Settings/serial, not on the picker as of 2026-08-14

## Module Overview

The BH-880 is a combined GNSS + magnetometer module and is the primary/recommended module. It uses the
same UART pins (GPIO43/44) as the original LC76G GPS-only module it initially replaced.

**LC76G support was brought back 2026-08-10** (see [ADR-0032](adr/0032-pinned-gps-module-not-always-auto-detect.md))
so a board built with either module works from the same firmware — see "Multi-Module Support" below.
A board running the LC76G has no compass, so navigation is forced to North-Up automatically (there's
nothing to rotate a heading-up view by); this is a real, permanent hardware constraint of that module,
not a bug or a temporary limitation.

**BN-880 support was added 2026-08-11** (ADR-0032's addendum, GitHub issue #1) after a field report:
BN-880 is a visually/name-similar but different module from the BH-880, sourced by name rather than by
the specific Beitian part. It speaks the same NMEA/PAIR protocol as the LC76G (no new GPS parser
needed — the existing two-pass `detectBaud()` finds it via the NMEA pass unmodified), but commonly
carries an **HMC5883L** magnetometer (I2C `0x1E`) instead of the BH-880's QMC5883L (`0x0D`) — so unlike
the LC76G, a BN-880 board *does* get heading-up navigation, just via a different compass chip and
driver. See "Compass: HMC5883L Magnetometer (BN-880)" below.

**BE-881 support was added 2026-08-14** after a user connected a board sold as "BE-880"/"BE-881" and
initially picked the BH-880 profile as the closest match — GPS worked (same B1301N-family UBX
protocol as BH-880, reuses that init path unchanged) but the compass failed at `0x0D`. BE-881 carries
a **QMC5883P** magnetometer (I2C `0x2C`) — a third, genuinely different chip despite the QMC5883L
naming similarity (different chip-ID/status/control register layout, confirmed against QST's official
datasheet). See "Compass: QMC5883P Magnetometer (BE-881)" below.

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

## Multi-Module Support: BH-880 + LC76G + BN-880 + BE-881

One firmware image supports all four modules. GPS protocol is auto-identified from the byte stream at
first boot and then pinned as a persisted choice — not re-detected on every boot. BN-880 reuses the
LC76G NMEA/PAIR path exactly; BE-881 reuses the BH-880 UBX path exactly. The only per-module difference
GPS detection itself needs to know about is UBX (BH-880, BE-881) vs. NMEA (LC76G, BN-880). Full design
rationale, alternatives considered, why detection had to be two strict sequential passes rather than one
interleaved pass, and the BN-880 addendum:
[ADR-0032](adr/0032-pinned-gps-module-not-always-auto-detect.md).

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

### Module pinning (Settings > GPS, or serial)

Detection above only ever runs on a board that hasn't been configured yet. `settings_manager` persists
`gps_module_type` (0=BH-880, 1=LC76G, 2=BN-880, 3=BE-881) and `gps_module_configured` (NVS, survives both the
OTA-only and full-flash web-flasher images — see [`docs/firmware_installation.md`](firmware_installation.md)
for why NVS's `0x9000-0xd000` region is never touched by either). Once pinned, `device_manager::initGPS()`
calls `gps_bh880::beginWithProtocol()` instead of `begin()` — skips protocol detection entirely and
runs only the single relevant pass (`detectBaudUBX()` or `detectBaudNMEA()` directly) for baud, since
baud can still vary per unit/config even when the module type is known. `gps_bh880::moduleFromType()`/
`moduleTypeName()` are the single source of truth for the `gps_module_type` -> `GpsModule` mapping and
its display name — every call site (device_manager, main.cpp's picker, diagnostics.cpp) shares them
rather than each inlining its own switch/ternary, after a duplicated inline mapping caused a real bug
(`beginWithProtocol()` briefly misclassified `BN880_NMEA` as UBX before this existed).

**Compass chip selection is never auto-probed — only ever a direct consequence of the pinned module,
with no fallback.** `device_manager::initCompass()` doesn't attempt any chip at all until
`gps_module_configured` is true (compass is simply unavailable on the very first, unconfigured boot);
once pinned, it goes straight to QMC5883L (BH-880), HMC5883L (BN-880), QMC5883P (BE-881), or skips
entirely (LC76G) — see
[ADR-0032](adr/0032-pinned-gps-module-not-always-auto-detect.md)'s 2026-08-11 addendum for why a
"try one chip, fall back to the other" shortcut was rejected even for the first-boot case.

**First boot**: before `gps_module_configured` is ever true, a one-time full-screen picker appears
(`main.cpp`, right after `ui_manager::init()`, before the loading-screen sequence) with three buttons —
**BH-880 / BE-881 / LC76G as of 2026-08-14**, showing what the GPS auto-detect scan found this boot as
a hint but always requiring an explicit tap. This set is a deliberately curated subset: it's the three
modules with real field verification, not the full list of modules the firmware supports. **BN-880 was
swapped out of the picker for BE-881 the same day BE-881 got field-verified** — BN-880's code path
(`GpsModule::BN880_NMEA`, `compass_hmc5883l.cpp`) was not removed, it's just no longer offered here
until it gets equivalent field verification; it remains reachable via Settings > GPS or
`gps module set bn880`. GPS itself isn't gated by this picker — it already came up via the two-pass
auto-detect earlier in boot (before display was even ready). **Picking a module reboots immediately**
(`esp_restart()` inside `showGpsModulePickerBlocking()`) — not just a preference save. This is required
for the compass, not GPS: `initCompass()` runs during Phase 2, before this picker can possibly have
shown, so on an unconfigured first boot it always skips compass entirely (no auto-probe); without the
reboot, a freshly-picked BH-880/BE-881 board would finish its first session with the correct pin saved
but a genuinely uninitialized compass (field-caught 2026-08-11 — see ADR-0032's addendum). The same
reasoning applies to `gps module set` on serial, which is why that path reboots too.

**Changing the pin later**: Settings > GPS has a module dropdown (options in enum order: BH-880,
LC76G, BN-880, BE-881) + "Save + Reboot to Apply" button (same `esp_restart()` pattern as the WiFi/AP
screens). The tab's old Hot/Warm/Cold Start and Factory Reset touch buttons were removed in an earlier
change — the web flasher gives full reflash control now, so those are redundant UI weight; the
underlying commands are unaffected and still reachable via `gps restart hot|warm|cold` and `gps reset`
on serial. **Also reachable entirely from serial** (`src/utils/diagnostics.cpp`, added alongside the
BN-880 work as a touch-independent fallback — the board that reported the BN-880 compass issue also
had a dead touch controller): `gps module` (show current pin), `gps module set bh880|lc76g|bn880|be881`
(pin + reboot), `gps module reset` (clear the pin so the picker shows again next boot).

Switching the pinned module resets stored compass calibration (`settings_manager::saveGPSModuleSelection()`
/ `resetGPSModuleConfiguration()`) — hard-iron offsets and the H0 baseline are specific to one physical
chip on one physical board, and silently carrying one chip's calibration over to a different chip after
a module switch would corrupt heading output rather than just read as "uncalibrated".

### No compass on LC76G — North-Up is forced automatically

`device_manager`'s `compass_ok` flag (already false-safe: `initCompass()` pings the I2C address before
ever attempting a real register read, so a genuinely absent chip fails cleanly with no bus errors or
retries) drives navigation mode, generically — this check has no idea which GPS module is pinned, only
whether compass init actually succeeded, so it needs no changes as compass-bearing module options are
added. If `compass_ok` is false at `ui_manager::init()`, `heading_up_mode` is forced off in RAM (not
written to NVS — a saved Heading-Up preference from a BH-880/BN-880 session is preserved and reapplied
if the compass is ever present again), and the Settings nav-mode dropdown is locked to North-Up
(`LV_STATE_DISABLED`) instead of offering a Heading-Up option that would silently do nothing. BN-880
and BE-881 boards both have a real compass (HMC5883L / QMC5883P respectively) and get Heading-Up like
a BH-880 board does — only LC76G forces North-Up.

### Key files added/changed for multi-module support

| File | Role |
|------|------|
| `src/hardware/sensors/gps_bh880.cpp` | NMEA/PAIR parser ported in alongside UBX; two-pass `detectBaud()`; `beginWithProtocol()`; `moduleFromType()`/`moduleTypeName()` |
| `include/hardware/sensors/gps_bh880.h` | `GpsModule` enum (BH880_UBX/LC76G_NMEA/BN880_NMEA/BE881_UBX), `beginWithProtocol()`, `isNmeaProtocol()`/`protocolName()` |
| `src/hardware/sensors/compass_hmc5883l.cpp` / `.h` | Low-level HMC5883L register driver (BN-880's compass chip) |
| `src/hardware/sensors/compass_qmc5883p.cpp` / `.h` | Low-level QMC5883P register driver (BE-881's compass chip) |
| `src/hardware/sensors/compass_qmc5883l.cpp` / `.h` | Public compass entry point for the whole codebase; dispatches internally to QMC5883L, HMC5883L, or QMC5883P via `ChipType` |
| `src/hardware/i2c/i2c_manager.cpp` / `.h` | `COMPASS_DEVICE_HMC` (0x1E) + `COMPASS_DEVICE_QMCP` (0x2C) device handles, `NUM_DEVICES` raised to 8 |
| `src/core/device_manager.cpp` | `initGPS()`/`initCompass()` branch on `gps_module_configured` + pinned type, no auto-probing |
| `src/utils/settings_manager.cpp` / `.h` | `gps_module_type`/`gps_module_configured`, `saveGPSModuleSelection()`, `resetGPSModuleConfiguration()`, compass-cal reset on module change |
| `src/core/main.cpp` | First-boot picker (`showGpsModulePickerBlocking()`), 3 buttons: BH-880/BE-881/LC76G (BN-880 removed from the picker 2026-08-14, code kept — see above) |
| `src/ui/ui_manager.cpp` | Forces North-Up when `!compass_ok` (unchanged — already module-agnostic) |
| `src/ui/settings_screen.cpp` | GPS Module dropdown (3 options) + reboot button; nav-mode dropdown disabled when no compass |
| `src/utils/diagnostics.cpp` | `gps module [set\|reset]` serial commands |

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

## Compass: HMC5883L Magnetometer (BN-880)

The BN-880 module commonly carries an HMC5883L rather than the BH-880's QMC5883L — a different chip
with a different register map, different byte order, and a different I2C address. Added 2026-08-11
(GitHub issue #1) as a low-level driver (`compass_hmc5883l.cpp`/`.h`) that `compass_qmc5883l.cpp`
dispatches to internally when the pinned module is BN-880 — see "Internal dispatch, not a public
rename" in the ADR list for why the public entry point kept its QMC5883L-named namespace regardless.

### Hardware Details

| Feature | Value |
|---------|-------|
| **Chip** | HMC5883L |
| **I2C Address** | 0x1E |
| **I2C Bus** | Shared (SDA=15, SCL=7, 100kHz) |
| **Identification Registers** | 0x0A/0x0B/0x0C, expected `'H'`/`'4'`/`'3'` (0x48/0x34/0x33) |
| **Output** | 16-bit signed X, Y, Z (magnetic field) — **burst order on the wire is X, Z, Y**, not X,Y,Z |
| **Byte order** | Big-endian (MSB first) per axis — the opposite of the QMC5883L's little-endian burst |
| **Field Range** | ±1.3 Gauss (1090 LSB/Gauss), datasheet default gain used |
| **ODR** | 15 Hz (8-sample averaging, normal measurement mode) |
| **Overflow** | No status bit — a data register reading exactly -4096 means that axis' ADC saturated |

**Two classic HMC5883L porting mistakes this driver deliberately does not make** by not copy-pasting
the QMC5883L decode: the X,Z,Y burst order (not X,Y,Z), and big-endian axis bytes (not little-endian).

### Register Map

| Register | Address | Purpose |
|----------|---------|---------|
| CONFIG_A | 0x00 | Sample averaging, output rate, measurement mode |
| CONFIG_B | 0x01 | Gain |
| MODE | 0x02 | 0x00 = continuous measurement |
| DATA_X_MSB / DATA_X_LSB | 0x03 / 0x04 | X-axis data |
| DATA_Z_MSB / DATA_Z_LSB | 0x05 / 0x06 | Z-axis data (note: comes before Y on the wire) |
| DATA_Y_MSB / DATA_Y_LSB | 0x07 / 0x08 | Y-axis data |
| STATUS | 0x09 | Bit 0 = RDY |
| IDENT_A / IDENT_B / IDENT_C | 0x0A / 0x0B / 0x0C | Fixed `'H'`/`'4'`/`'3'` |

### Initialization Sequence

1. Read identification registers 0x0A/0x0B/0x0C, verify `'H'`/`'4'`/`'3'`.
2. Write `0x70` to CONFIG_A (8-sample averaging, 15 Hz, normal mode).
3. Write `0x20` to CONFIG_B (±1.3 Ga gain — datasheet default).
4. Write `0x00` to MODE (continuous measurement).

### Calibration, health classification, and heading

All chip-agnostic — the same hard-iron calibration, EMA health classification
(`compass_qmc5883l::classifyHealth()`), and 2-axis `atan2f(cy, cx)` heading formula used for the
QMC5883L apply unchanged once `compass_hmc5883l::readRaw()` has populated x/y/z/overflow. See
[`docs/compass.md`](compass.md) for the full pipeline — nothing there is HMC5883L-specific.

### Key Code Files

| File | Purpose |
|------|---------|
| `include/hardware/sensors/compass_hmc5883l.h` / `src/hardware/sensors/compass_hmc5883l.cpp` | Low-level register driver |
| `include/hardware/i2c/i2c_manager.h` | `COMPASS_DEVICE_HMC` handle (0x1E) |
| `include/hardware/sensors/compass_qmc5883l.h` | `ChipType` enum, dispatch entry point |

---

## Compass: QMC5883P Magnetometer (BE-881)

The BE-881 module carries a QMC5883P rather than the BH-880's QMC5883L — despite the near-identical
name, a genuinely different chip: different chip-ID register, different status register, different
control-register layout, and a different I2C address. Added 2026-08-14 after a field report (board
sold as "BE-880"/"BE-881", GPS worked fine via the BH-880 UBX path but the compass failed at `0x0D`).
Identified positively via the official QST QMC5883P datasheet plus a live double-ACK-confirmed bus
scan hit at `0x2C`. Low-level driver is `compass_qmc5883p.cpp`/`.h`, dispatched to internally from
`compass_qmc5883l.cpp` — same pattern as the HMC5883L above, same reasoning for keeping the public
entry point's name (see "Internal dispatch, not a public rename" in the ADR list).

### Hardware Details

| Feature | Value |
|---------|-------|
| **Chip** | QMC5883P |
| **I2C Address** | 0x2C |
| **I2C Bus** | Shared (SDA=15, SCL=7, 100kHz) |
| **Identification Register** | 0x00, expected `0x80` |
| **Output** | 16-bit signed X, Y, Z (magnetic field), little-endian, native X,Y,Z burst order |
| **Field Range** | ±8 Gauss (configured; chip also supports ±2G/±12G/±30G) |
| **ODR** | 200 Hz continuous, OSR1=8 |
| **Overflow** | STATUS register (0x09) bit 1 (OVFL); bit 0 is DRDY |
| **Undocumented sign register** | 0x29 — required by the datasheet's own setup examples but absent from its main register table; written `0x06` at init |

### Register Map

| Register | Address | Purpose |
|----------|---------|---------|
| CHIP_ID | 0x00 | Fixed `0x80` |
| DATA | 0x01-0x06 | X_LSB, X_MSB, Y_LSB, Y_MSB, Z_LSB, Z_MSB |
| STATUS | 0x09 | Bit 0 = DRDY, bit 1 = OVFL |
| CONTROL1 | 0x0A | OSR2 / OSR1 / ODR / MODE |
| CONTROL2 | 0x0B | SOFT_RST / SELF_TEST / RNG / SET-RESET-MODE |
| SIGN | 0x29 | Axis-sign register (undocumented in the register table; datasheet setup examples require it) |

### Initialization Sequence

1. Read CHIP_ID (0x00), verify `0x80`.
2. Write `0x06` to SIGN (0x29) — required by the datasheet's own setup examples.
3. Write `0x08` to CONTROL2 (0x0B) — 8G range.
4. Write `0x0F` to CONTROL1 (0x0A) — continuous mode, 200Hz ODR, OSR1=8.

### Calibration, health classification, and heading

All chip-agnostic — the same hard-iron calibration, EMA health classification, and 2-axis
`atan2f(cy, cx)` heading formula used for the QMC5883L/HMC5883L apply unchanged once
`compass_qmc5883p::readRaw()` has populated x/y/z/overflow. See [`docs/compass.md`](compass.md) for the
full pipeline — nothing there is QMC5883P-specific.

**Known cosmetic issue**: `compass read`'s serial output converts the raw horizontal magnitude to µT
using a `120 LSB/µT` constant that was derived for the QMC5883L's 2G range. At the QMC5883P's 8G range
the printed µT figure is mis-scaled (reads low); heading itself is unaffected since it's an angle
(`atan2f`), not a magnitude, and is scale-independent. Not fixed as of 2026-08-14 — cosmetic only.

### Key Code Files

| File | Purpose |
|------|---------|
| `include/hardware/sensors/compass_qmc5883p.h` / `src/hardware/sensors/compass_qmc5883p.cpp` | Low-level register driver |
| `include/hardware/i2c/i2c_manager.h` | `COMPASS_DEVICE_QMCP` handle (0x2C) |
| `include/hardware/sensors/compass_qmc5883l.h` | `ChipType` enum, dispatch entry point |

**Note**: the older `compass status`/`compass init` serial commands (`src/utils/diagnostics.cpp`) poke
QMC5883L registers at `0x0D` directly and pre-date the multi-chip dispatch — they always report "NOT
FOUND" on a BE-881 board. Use `compass read` for verification; it goes through the real driver dispatch
and reflects whichever chip is actually pinned.

---

## I2C Bus Devices (Complete)

All devices on the shared I2C bus (SDA=15, SCL=7 @ 100kHz):

| Address | Device | Source |
|---------|--------|--------|
| 0x0D | QMC5883L (Compass) | BH-880 module |
| 0x15 | CST820 (Touch) | Waveshare board |
| 0x1E | HMC5883L (Compass) | BN-880 module |
| 0x20 | TCA9554 (IO Expander) | Waveshare board |
| 0x2C | QMC5883P (Compass) | BE-881 module |
| 0x51 | PCF85063 (RTC) | Waveshare board |
| 0x6A | QMI8658 (IMU, low address) | Waveshare board |
| 0x6B | QMI8658 (IMU, high address) | Waveshare board |

All three compass device handles (`COMPASS_DEVICE`, `COMPASS_DEVICE_HMC`, `COMPASS_DEVICE_QMCP`) are
registered on the bus unconditionally at boot, like every other device here — only the pinned module's
driver ever actually talks to its chip (see "Module pinning" above); registering the others' handles is
harmless since `i2c_master_bus_add_device()` just reserves an address slot, it doesn't probe.

A scan will sometimes also show `0x7E` — that's not a real device, it's a probe artifact
at a reserved I2C address that survives even a double-ACK confirmation guard. See
[`docs/i2c_bus_freeze_investigation.md`](i2c_bus_freeze_investigation.md) for detail.

---

**Compass software implementation**: See [`docs/compass.md`](compass.md) for heading pipeline, calibration, WMM declination, I2C constraints, and upgrade path.

*Last updated: 2026-08-11 (BN-880 support added — see "Multi-Module Support" and "Compass: HMC5883L
Magnetometer" above, [ADR-0032](adr/0032-pinned-gps-module-not-always-auto-detect.md)'s addendum, and
CHANGELOG.md)*
