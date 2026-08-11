#pragma once
#include "core/arduino_compat.h"

// Low-level HMC5883L register driver (the compass chip found on many BN-880 GPS/compass
// modules, as distinct from the QMC5883L on the Beitian BH-880 this firmware otherwise
// targets — see docs/adr/0032-pinned-gps-module-not-always-auto-detect.md's BN-880
// addendum). Intentionally minimal: chip register I/O only, no calibration or health
// classification. compass_qmc5883l.cpp is the single public compass entry point used
// everywhere else in the codebase; it dispatches here internally when the pinned GPS
// module is BN-880. Do not call this namespace directly from outside that file.
namespace compass_hmc5883l {

    bool begin();     // Verify chip ID (0x48/0x34/0x33), configure continuous mode, +/-1.3 Ga
    bool isReady();    // Data-ready bit in the status register

    // Raw axis values, already reordered into X/Y/Z (the chip's own burst order is X,Z,Y —
    // see the comment in the .cpp). overflow is true if any axis hit the ADC's saturation
    // sentinel (-4096 per datasheet), not a hardware-reported single-bit flag like the
    // QMC5883L's STATUS_OVL.
    bool readRaw(int16_t& x, int16_t& y, int16_t& z, bool& overflow);

}
