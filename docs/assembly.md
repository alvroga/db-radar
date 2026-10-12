# Assembly Instructions

Wiring and soldering guide for connecting the GPS/compass module and the DC boost converter to the
Waveshare ESP32-S3-Touch-LCD-2.1 board. For the enclosure and print settings see the
[README](../README.md#3d-enclosure); for the full build sequence (seating the board, mounting the
battery, closing the case) see [`docs/manual.md`](manual.md#building-one).

> **Status of the numbers below.** Pin functions, GPIO numbers and the BH-880 module data come from
> this repository (`bh880_module.md`, `system_config.h`, `battery_monitoring.md`). Items marked
> **⚠️ Verify** are *not* documented in the repo and must be checked against your own parts before
> soldering.

---

## 1. Parts and dimensions

| Part | Dimensions | Notes |
|------|-----------|-------|
| Waveshare ESP32-S3-Touch-LCD-2.1 | 2.1" round display, 480×480 | Mechanical drawing: see Waveshare's documentation |
| BH-880 GPS + compass module | 28 × 28 × 11 mm, 12 g | 1.25 mm pitch, 6-pin connector |
| DC boost converter (MT3608) | ⚠️ Verify — typically ~36 × 17 mm PCB, ~14 mm tall | Not specified in the repo; varies by manufacturer. Measure yours with calipers and check it fits the enclosure |
| LiPo battery | 3.7 V, 1500 mAh recommended | MX1.25 2-pin connector (J1 on the board) — plug in, no soldering |

**Tools:** fine-tip soldering iron, flux-core solder, thin flexible wire (AWG 26–28), multimeter,
heat-shrink tubing or Kapton tape, tweezers.

---

## 2. Pinouts

### 2.1 BH-880 module (1.25 mm, 6-pin connector)

| Module pin | Label | Connects to | Function |
|-----------|-------|-------------|----------|
| 1 | D (SDA) | ESP32-S3 **GPIO 15** | I2C data (compass) |
| 2 | G (GND) | GND | Ground |
| 3 | T (TX) | ESP32-S3 **GPIO 44** (ESP RX) | GPS UART TX |
| 4 | R (RX) | ESP32-S3 **GPIO 43** (ESP TX) | GPS UART RX |
| 5 | V (VCC) | **Boost converter OUT+ (5 V)** | Power, 3.6–5.5 V |
| 6 | C (SCL) | ESP32-S3 **GPIO 7** | I2C clock (compass) |

UART runs at 115200 baud. The I2C bus (SDA = 15, SCL = 7, 100 kHz) is shared with the touch
controller, RTC, IO expander and IMU already on the board.

> **⚠️ Verify** the pin numbering/orientation against your module's connector before soldering.

### 2.2 DC boost converter (MT3608-style)

| Pad | Function | Connects to |
|-----|----------|-------------|
| IN+ (VIN+) | Input positive | Board 3.3 V rail |
| IN− (VIN−) | Input negative | GND |
| OUT+ (VOUT+) | Output positive | BH-880 pin 5 (VCC) |
| OUT− (VOUT−) | Output negative | GND (common with the GPS) |

The module has a multi-turn trimmer that sets the output voltage. Pad positions and silkscreen vary
by manufacturer — **⚠️ Verify** them with a multimeter.

### 2.3 Other GPS modules (same firmware)

| Module | Compass | Wiring notes |
|--------|---------|--------------|
| **BE-881** | QMC5883P (I2C `0x2C`) | Same UBX protocol as the BH-880. Pin order is **not documented in the repo** — check your module's datasheet |
| **BN-880** | HMC5883L (I2C `0x1E`) | NMEA protocol. Pin order **not documented** — check your datasheet |
| **LC76G** | None | GPS only: no SDA/SCL needed, just TX, RX, VCC, GND. Navigation is locked to North-Up |

### 2.4 Board signals used

| Signal | GPIO |
|--------|------|
| GPS_TX (ESP → GPS RX) | 43 |
| GPS_RX (ESP ← GPS TX) | 44 |
| I2C_SDA | 15 |
| I2C_SCL | 7 |

> **⚠️ Verify** which physical pads on the Waveshare board carry 3.3 V, 5 V and GND using Waveshare's
> official pinout/schematic. The repo gives GPIO numbers but not the physical pad layout.

---

## 3. Wiring overview

```
                    ┌──────────────────────┐
  Board 3V3 ───────►│ IN+          OUT+    │────────► GPS pin 5 (VCC)
  Board GND ───────►│ IN−          OUT−    │────────► GPS pin 2 (GND)
                    └──────────────────────┘
                         DC boost (set to 5.0 V)

  GPIO 15 (SDA) ◄───────────────────────────────────► GPS pin 1 (D / SDA)
  GPIO 7  (SCL) ◄───────────────────────────────────► GPS pin 6 (C / SCL)
  GPIO 44 (RX)  ◄───────────────────────────────────  GPS pin 3 (T / TX)
  GPIO 43 (TX)  ────────────────────────────────────► GPS pin 4 (R / RX)

  All grounds (board, boost, GPS) joined at a common point.
```

Note that TX and RX are **crossed**: the GPS TX goes to the ESP RX and vice versa.

---

## 4. Step-by-step soldering

### Step 0 — Prepare

1. Print the enclosure and check the fit before installing anything (see the manual, step 1).
2. Measure the boost converter with calipers and confirm it fits alongside the GPS module and the LiPo.
3. Identify the boost converter's IN+/IN−/OUT+/OUT− pads with a multimeter.

### Step 1 — Set the boost converter to 5.0 V (before connecting the GPS)

1. Solder only the two input wires (IN+ to the 3.3 V source, IN− to GND), or feed the input from a
   bench supply set to 3.3 V.
2. Measure the output with a multimeter and turn the trimmer until it reads **5.0 V**.
   Never exceed **5.5 V** — that is the BH-880's maximum.
3. Disconnect. An unadjusted boost converter can output a much higher voltage and destroy the GPS.

### Step 2 — Prepare the GPS cable

1. Build a 6-wire cable (1.25 mm connector, or solder directly to the pads) following table 2.1.
2. Suggested colour code: red = VCC, black = GND, yellow = SDA, green = SCL, blue = GPS TX, white = GPS RX.
3. Check continuity pin-by-pin and look for bridges between neighbours — the 1.25 mm pitch leaves
   very little margin.

### Step 3 — Data lines to the board

1. SDA (pin 1) → GPIO 15.
2. SCL (pin 6) → GPIO 7.
3. GPS TX (pin 3) → GPIO 44, and GPS RX (pin 4) → GPIO 43.
4. Insulate each joint with heat-shrink tubing.

### Step 4 — Power through the boost converter

1. Input: IN+ → board 3.3 V rail, IN− → GND.
2. Output: OUT+ → GPS pin 5 (VCC), OUT− → GND.
3. Join all grounds (board, boost, GPS) at a common point.
4. **Before plugging in the GPS**, power the board and confirm OUT+ reads ~5.0 V.

### Step 5 — Verify

1. With no battery attached, power the board over USB-C and check the voltage at GPS pin 5.
2. Flash the firmware (see the [README](../README.md#installation)) and pick your module in the
   first-boot picker (BH-880 / BE-881 / LC76G).
3. Over serial, run `gps status` and `compass status` to confirm the GPS and compass respond.
4. Continue with steps 4–8 of the [manual](manual.md#building-one): seat the board, mount the LiPo,
   route the wiring so nothing pinches, and close the case.

Photo slots referenced by the manual: `02-wiring-module.jpg` and `03-wiring-boost-converter.jpg`.

---

## 5. Open questions to confirm before soldering

- **Power source:** the README and `bh880_module.md` describe powering the module from 5 V; the manual
  says to place the boost converter between the board's 3.3 V rail and the module's power pins. These
  are consistent (3.3 V in, 5 V out), but confirm where you tap the input and that the 3.3 V rail can
  supply the load (the BH-880 draws about 20 mA at 5 V, plus converter losses).
- **Logic levels:** the repo does not say whether the BH-880's TX/SDA/SCL swing to 3.3 V when the module
  is powered at 5 V. The ESP32-S3 GPIOs are not 5 V tolerant — measure before connecting.
- **Boost converter dimensions and pads:** not in the repo; measure your own unit.

---

Licensed under [CC BY-NC-SA 4.0](../LICENSE).
