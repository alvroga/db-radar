# Compass I2C Read Rate Constraint

**Status**: stale, unconfirmed either way. The original claim below — "compass reads must stay in the
System Task because the CST820 touch driver bypasses the I2C mutex" — described the pre-ESP-IDF
Arduino build. It does not describe the current firmware:

- There is no `Wire` usage anywhere in `src/`/`include/`.
- `cst820_read()` reads through `i2c_manager::read()` (`src/hardware/display/cst820.cpp`), exactly
  like the RTC, EXIO, and compass. Every one of those calls is serialized by `i2c_manager`'s recursive
  mutex (`i2c_manager.cpp`). There is no unprotected participant on the bus.

So the original reason the compass can't move to the I2C Task no longer applies. That doesn't mean the
constraint is void, either — nobody has re-tried moving the compass read to the I2C Task since the
migration, so it's untested under the current stack, not disproved. The compass currently reads at
10Hz from the System Task (see [`compass.md`](compass.md)) and this has not been a bottleneck.

**If anyone picks this up**: the cheap first step is a measurement, not a rewrite — move the compass
read to the I2C Task behind a runtime flag, watch `i2c_manager::getStats()`
(`total_ops`/`failed_ops`/`consecutive_failures`) and task-health output, and see whether anything
actually degrades. The per-device forensic counters added for the [I2C bus freeze
investigation](i2c.md#known-historical-issues-resolved) make this observable in a way it wasn't when
this constraint was first written.

Full detail on the current I2C architecture: [`i2c.md`](i2c.md). Related ADRs:
[0013](adr/0013-i2c-process-ms-tuned-floor.md), [0014](adr/0014-compass-stays-on-shared-i2c-bus.md).

The original Arduino-era analysis (dated, kept for historical context only) is preserved in git
history for this file as of 2026-08-07.
