# Beitian BH-880 GPS + Compass Module

**Status**: GPS ✅ Working | Compass ✅ Working (WMM declination applied)

## Module Overview

The BH-880 is a combined GNSS + magnetometer module that replaces the original LC76G GPS-only module. It is a drop-in replacement using the same UART pins (GPIO43/44) for GPS and adds a compass via the shared I2C bus.

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

| Feature | LC76G (old) | BH-880 (new) |
|---------|-------------|--------------|
| Protocol | NMEA text + PAIR commands | UBX binary |
| Baud detect | Look for `$` character | Look for 0xB5 0x62 sync |
| Config commands | PAIR001, PAIR062, etc. | UBX-CFG-RATE, CFG-RST, etc. |
| Default rate | 1 Hz | 10 Hz |
| GNSS systems | Configurable via PAIR066 | All enabled by default |
| Positioning mode | PAIR062 (normal/fitness/etc) | Not applicable |
| Compass | None | QMC5883L built-in |

---

## Compass: QMC5883L Magnetometer

The BH-880 includes a QMC5883L 3-axis magnetometer connected via I2C.

### Hardware Details

| Feature | Value |
|---------|-------|
| **Chip** | QMC5883L |
| **I2C Address** | 0x0D |
| **I2C Bus** | Shared (SDA=15, SCL=7, 400kHz) |
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
- This gives **magnetic heading**, not true heading
- Must apply **magnetic declination** for true north (location-dependent)
- The module orientation relative to the device matters - mounting angle offset may be needed
- **Calibration** (hard/soft iron compensation) is essential for accurate readings
- Tilt compensation requires accelerometer data (available from QMI8658 IMU on the board)

### Serial Commands

```
compass status          - Read chip ID and show compass status
compass read            - Read and display raw X/Y/Z values and computed heading
compass stream [s]      - Stream compass readings for N seconds (default 5)
```

### Calibration (Future)

For accurate compass readings, calibration is needed:

1. **Hard-iron calibration**: Rotate device 360 degrees, record min/max for each axis, compute offsets
2. **Soft-iron calibration**: Fit ellipse to XY data, compute scale factors
3. **Mounting offset**: Determine angular offset between compass X-axis and device "forward" direction
4. **Tilt compensation**: Use accelerometer (QMI8658 at 0x6B) to compensate for device tilt

### Key Code Files

| File | Purpose |
|------|---------|
| `include/hardware/i2c/i2c_manager.h` | COMPASS_DEVICE handle (0x0D) |
| `src/utils/diagnostics.cpp` | `compass` serial commands for testing |

---

## I2C Bus Devices (Complete)

All devices on the shared I2C bus (SDA=15, SCL=7 @ 400kHz):

| Address | Device | Source |
|---------|--------|--------|
| 0x0D | QMC5883L (Compass) | BH-880 module |
| 0x15 | CST820 (Touch) | Waveshare board |
| 0x20 | TCA9554 (IO Expander) | Waveshare board |
| 0x51 | PCF85063 (RTC) | Waveshare board |
| 0x6B | QMI8658 (IMU) | Waveshare board |
| 0x7E | Unknown | TBD |

---

**Compass software implementation**: See [`docs/compass.md`](compass.md) for heading pipeline, calibration, WMM declination, I2C constraints, and upgrade path.

*Last updated: 2026-03-18 (GPS + compass fully working, WMM declination active)*
