#pragma once
#include "core/arduino_compat.h"

// Low-level QMC5883P register driver (the compass chip on Beitian's BE-881/BH-881
// GPS+compass modules — the successor part to the QMC5883L on the BH-880, but NOT
// register-compatible with it despite the similar name: different I2C address (0x2C
// vs 0x0D), different chip-ID register/value, different status register address, and
// mode/ODR/range packed into the control registers differently. See QST datasheet
// 13-52-19 Rev. A. compass_qmc5883l.cpp is the single public compass entry point used
// everywhere else in the codebase; it dispatches here internally when the pinned GPS
// module is BE-881. Do not call this namespace directly from outside that file.
namespace compass_qmc5883p {

    // LSB per microtesla at the +/-2G range begin() configures (datasheet Table 2:
    // 15000 LSB/Gauss at +/-2G, 1 Gauss = 100uT -> 150 LSB/uT). Used by
    // compass_qmc5883l::lsbPerUt() for the 'compass read'/'compass cal' uT display —
    // must be updated together with begin()'s range if that ever changes.
    constexpr float LSB_PER_UT = 150.0f;

    bool begin();     // Verify chip ID (0x00 == 0x80), enter continuous mode 200Hz / 2G
    bool reset();      // Soft reset (CONTROL2 bit7) then re-run begin()
    bool isReady();    // DRDY bit in the status register (0x09, bit0)

    // Raw axis values, already in X/Y/Z order (data registers 0x01-0x06 are natively
    // X,Y,Z — no reordering needed, unlike the HMC5883L). overflow is the hardware OVFL
    // bit (status register 0x09, bit1).
    bool readRaw(int16_t& x, int16_t& y, int16_t& z, bool& overflow);

    // Prints chip presence/ID/mode/range/status for the 'compass status' serial command.
    // Self-contained (doesn't expose this file's register map) — diagnostics.cpp just
    // dispatches to this based on compass_qmc5883l::activeChip().
    void debugPrintStatus();

}
