# Compass I2C Read Rate Constraint

## The Problem

Increasing compass (QMC5883L) read frequency by moving it from the System Task to the I2C Task immediately causes:

```
[E][Wire.cpp:499] requestFrom(): i2cWriteReadNonStop returned Error -1
```

Followed by system instability, UI freezes, and eventual crash.

## Root Cause: Shared Bus + Unprotected Touch Reads

All I2C devices share one bus on GPIO15 (SDA) / GPIO7 (SCL) at 400kHz:

| Device | Address | Who reads it |
|--------|---------|-------------|
| CST820 touch | 0x15 | LVGL driver, called from UI Task at ~60Hz |
| PCF85063 RTC | 0x51 | System Task via I2C queue |
| TCA9554 EXIO | 0x20 | Various, via I2C queue |
| QMC5883L compass | 0x0D | System Task directly |

The LVGL touch driver calls `Wire.requestFrom(0x15, ...)` directly — it does **not** go through `i2c_manager` and does **not** acquire `i2c_mutex`. This means touch reads are invisible to the mutex-based serialisation that protects RTC and EXIO operations.

## Why the I2C Task Fails

The I2C Task runs every **20ms**. With the compass read there at 100ms intervals, a read attempt happens every 100ms. At 400kHz with an LVGL touch poll rate of ~16ms (60Hz), the probability of a collision on any given compass read is high. The result is `requestFrom() Error -1` — the bus is mid-transaction when the compass tries to start its own.

The System Task runs every **5000ms**. The 100ms internal timer fires, but the code block is only *reached* once per 5-second loop iteration, meaning the compass actually reads at ~0.2Hz. At this low rate the collision window is small enough that errors are extremely rare.

## Why the Mutex Does Not Help

`i2c_manager::read()` acquires `i2c_mutex` before touching the bus. But the LVGL CST820 driver bypasses `i2c_manager` entirely — it calls `Wire` directly. No mutex is taken. There is no way to make the touch reads respect the mutex without modifying the vendor LVGL touch driver.

## The Real Fix: Physical Bus Isolation

The only reliable solution is to move the compass wires to a second I2C bus:

- **Wire** (GPIO15/7): touch + RTC + EXIO — existing shared bus
- **Wire1** (GPIO19/20): compass only — dedicated, zero contention

With isolation, compass can be read from the I2C Task at full 20ms rate (50Hz updates) with no collisions possible. The `cc-radar-compass` build environment and `i2c_manager::setCompassBus(&Wire1)` infrastructure are already implemented for this purpose.

## Current Workaround

Compass reads remain in the **System Task** at effective ~0.2Hz (once per 5-second loop). The 100ms timer in the code is aspirational but meaningless given the loop rate. Heading updates are slow (~5s reaction time) but stable.

## What Was Tried and Failed

Moving the compass block verbatim from `systemTask()` to `i2cTask()` — same code, different task context. Immediate `requestFrom Error -1` on first run. Reverted same session.

## Path Forward

1. Move compass SDA → GPIO19, SCL → GPIO20
2. Upload `cc-radar-compass` build (`pio run -e cc-radar-compass -t upload`)
3. Move compass read from System Task to I2C Task (or increase System Task loop rate)
4. Reaction time drops from ~5s to ~100ms
