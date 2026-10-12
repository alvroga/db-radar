# Bill of Materials

Parts list for building a Dragon Ball Radar. It covers the **Round** variant, which is the one
documented in detail in this repository. The rectangular **Flat** variant (320×240 / 240×320) is
listed at the end as a placeholder.

> **Status of the data below.** Part identities, voltages, connectors and sizes come from this
> repository (`README.md`, `manual.md`, `bh880_module.md`, `battery_monitoring.md`, `CHANGELOG.md`,
> `system_config.h`). Items marked **⚠️ Verify** are *not* specified in the repo — confirm them against
> the part you actually buy. No prices or supplier links are given: the repo has none and they change
> often.

See [`docs/assembly.md`](assembly.md) for wiring and soldering, and the
[README's GPS Module Options](../README.md#gps-module-options) for choosing a GPS module.

---

## Round variant — Waveshare ESP32-S3-Touch-LCD-2.1 (480×480)

### Required

| # | Part | Qty | Key specs | Notes |
|---|------|-----|-----------|-------|
| 1 | **Waveshare ESP32-S3-Touch-LCD-2.1** | 1 | ESP32-S3 @ 240 MHz, 16 MB flash, 8 MB PSRAM, 2.1" 480×480 round IPS LCD (ST7701), capacitive touch (CST820), WiFi + Bluetooth, USB-C | Includes the RTC (PCF85063), IO expander (TCA9554), IMU (QMI8658), buzzer and battery charger (ETA6098) used by the firmware |
| 2 | **GPS module** — pick **one** (see below) | 1 | UART on GPIO 43/44 | BH-880 is the primary/recommended module |
| 3 | **DC boost converter** (MT3608) | 1 | 3.3 V in → 5 V out, trimmer-adjustable | Needed because the BH-880 requires 3.6–5.5 V. **⚠️ Verify** size (typically ~36 × 17 mm) and pad layout; not in the repo |
| 4 | **LiPo battery** | 1 | 3.7 V, 1500 mAh recommended (tested), **MX1.25 2-pin** connector (J1) | Check polarity against the board's J1 before plugging in |
| 5 | **Enclosure** (3D printed) | 1 set | PLA or PETG, 0.2 mm layers, 20% infill | [Dragon Ball Radar on MakerWorld](https://makerworld.com/en/models/2776400-dragon-ball-radar); USB-C cutout and LiPo recess underneath |
| 6 | **M2 screws** | per enclosure design | — | Close the enclosure (manual, step 7). **⚠️ Verify** count and length against the model |
| 7 | **M1.4 screws** | per enclosure design | — | Secure internal components (manual, step 7). **⚠️ Verify** count and length |
| 8 | **Hookup wire** | ~1 m | Thin, flexible, AWG 26–28 | 6 conductors for the GPS cable + power leads for the boost converter |
| 9 | **USB-C data cable** | 1 | Data-capable (not charge-only) | For flashing and serial console |

### GPS module options (choose one)

| Module | Type | Compass | Protocol | Power | Notes |
|--------|------|---------|----------|-------|-------|
| **BH-880** (Beitian) | GPS + compass | QMC5883L (I2C `0x0D`) | UBX binary | 3.6–5.5 V, ~20 mA | Primary. 28 × 28 × 11 mm, 12 g, 1.25 mm 6-pin connector. Enables Heading-Up |
| **BE-881** | GPS + compass | QMC5883P (I2C `0x2C`) | UBX binary | **⚠️ Verify** (same family as BH-880) | Field-verified. May be sold as "BE-880". Pin order not documented in the repo |
| **LC76G** | GPS only | None | NMEA / PAIR | **⚠️ Verify** | Field-verified. Simpler wiring (no I2C); navigation locked to North-Up |
| **BN-880** | GPS + compass | HMC5883L (I2C `0x1E`) | NMEA / PAIR | **⚠️ Verify** | Supported in firmware but not field-verified; not on the first-boot picker (set via Settings or serial) |

### Optional

| Part | Qty | Purpose | Notes |
|------|-----|---------|-------|
| **microSD card** | 1 | Developer-mode logging only (`system_logger`, field log) | GPX waypoints are stored in on-board flash, not on the SD card. **⚠️ Verify** accepted card size/format |
| **BLE tag (legacy advertising)** | 1+ | Beacon proximity ("find your own Drac ball") | Must advertise in **Legacy BLE 4.0** mode; BLE 5.0 extended advertising is invisible to the radar |
| **Heat-shrink tubing / Kapton tape** | as needed | Insulating solder joints | Recommended when wiring the 1.25 mm connector |
| **1.25 mm 6-pin connector + pigtail** | 1 | Plug-in connection to the GPS module | Alternative to soldering wires directly to the module pads |

### Tools

Fine-tip soldering iron, flux-core solder, tweezers, multimeter (needed to set the boost converter to
5.0 V and verify continuity), calipers (to check the boost converter fits), 3D printer (or a print service).

---

## Wiring summary (what each part connects to)

| From | To |
|------|----|
| GPS SDA / SCL | ESP32-S3 GPIO 15 / GPIO 7 (shared I2C bus) |
| GPS TX / RX | ESP32-S3 GPIO 44 (RX) / GPIO 43 (TX) |
| Boost converter IN+ / IN− | Board 3.3 V rail / GND |
| Boost converter OUT+ / OUT− | GPS VCC (5 V) / GND |
| LiPo | Board J1 (MX1.25) |

Full detail and step-by-step soldering: [`docs/assembly.md`](assembly.md).

---

## Flat variant — rectangular display (320×240 / 240×320)

**Not yet documented.** The firmware supports a second hardware variant selected by a single
`#define` in `include/core/system_config.h` (see [`docs/configuration.md`](configuration.md)), but the
repository does not list the specific board, display or pin assignments for it. The GPS, boost
converter, battery and enclosure-independent parts above are expected to carry over, but this has
not been confirmed — **⚠️ Verify** before ordering.

---

Licensed under [CC BY-NC-SA 4.0](../LICENSE).
