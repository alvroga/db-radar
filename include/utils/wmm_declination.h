#pragma once
#include <stdint.h>

namespace wmm {

/**
 * Compute magnetic declination using WMM2020 model (n=1..3 truncation).
 * Accuracy: ±1° for mid-latitudes (North America, Europe), ±2° globally.
 * WMM2020 coefficients with secular variation — extrapolates to ~2027 with <0.5° error.
 *
 * @param lat_deg  Geographic latitude  (-90 to +90, positive = North)
 * @param lon_deg  Geographic longitude (-180 to +180, positive = East)
 * @param alt_m    Altitude above WGS84 ellipsoid in meters (0 for ground level)
 * @param year     Decimal year, e.g. 2026.2 for mid-March 2026
 * @return         Declination in degrees, positive = East
 *                 Apply: true_heading = magnetic_heading - declination
 */
float computeDeclination(float lat_deg, float lon_deg, float alt_m, float year);

/**
 * Convert a Unix timestamp (seconds since 1970-01-01) to decimal year.
 */
float toDecimalYear(uint32_t unix_timestamp);

} // namespace wmm
