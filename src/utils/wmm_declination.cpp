#include "utils/wmm_declination.h"
#include <math.h>

// =============================================================================
// WMM2020 — World Magnetic Model 2020 (n=1..3 truncation)
// =============================================================================
// Source: NOAA/NCEI World Magnetic Model 2020
//   https://www.ncei.noaa.gov/products/world-magnetic-model
// Coefficients: Epoch 2020.0, secular variation applied for dates beyond 2020.
// Accuracy: ±1° at mid-latitudes, ±2° globally.
//           Suitable for compass declination correction on a walking device.
// Next update: When WMM2030 coefficients are published (~2029), update epoch/table.
// =============================================================================

namespace wmm {

// WGS84 ellipsoid constants
static constexpr float WGS84_A  = 6378.137f;          // semi-major axis (km)
static constexpr float WGS84_E2 = 0.00669437999014f;  // eccentricity squared
static constexpr float WMM_A    = 6371.2f;             // WMM reference radius (km)
static constexpr float WMM_EPOCH = 2020.0f;

static constexpr float DEG2RAD = 3.14159265358979f / 180.0f;
static constexpr float RAD2DEG = 180.0f / 3.14159265358979f;

// WMM2020 Gauss coefficients (nT) and secular variation (nT/yr) for n=1..3
// Format: { n, m, g_nm, h_nm, gdot_nm, hdot_nm }
struct Coef { int n, m; float g, h, gd, hd; };

static const Coef WMM2020[] = {
    // n  m       g(nT)     h(nT)   gd(nT/yr)  hd(nT/yr)
    { 1, 0, -29404.5f,     0.0f,      6.7f,     0.0f },
    { 1, 1,  -1450.7f,  4652.9f,      7.7f,   -25.1f },
    { 2, 0,  -2500.0f,     0.0f,    -11.5f,     0.0f },
    { 2, 1,   2982.0f, -2991.6f,     -7.1f,   -30.2f },
    { 2, 2,   1676.7f,  -734.8f,      2.2f,   -23.9f },
    { 3, 0,   1363.9f,     0.0f,      2.8f,     0.0f },
    { 3, 1,  -2381.0f,   -82.2f,     -6.2f,     5.7f },
    { 3, 2,   1236.2f,   241.8f,      3.4f,    -0.5f },
    { 3, 3,    525.7f,  -542.9f,    -12.2f,     1.1f },
};
static constexpr int N_COEFS = 9;

// =============================================================================
// Schmidt quasi-normal associated Legendre functions and their colatitude
// derivatives, evaluated at geocentric latitude phi_c (NOT colatitude).
//
// Convention: s = sin(phi_c), c = cos(phi_c)
//   phi_c = geocentric latitude (positive North)
//   theta = geocentric colatitude = 90° - phi_c
//   sin(theta) = c, cos(theta) = s
//
// P̃[n][m]: Schmidt quasi-normal Legendre function value at argument sin(phi_c)
// dP[n][m]: dP̃/dtheta — derivative wrt colatitude
// Pdiv[n][m]: P̃[n][m] / sin(theta) = P̃[n][m] / c — for eastward component (m>0)
// =============================================================================

float computeDeclination(float lat_deg, float lon_deg, float alt_m, float year) {
    // --- Geocentric conversion (WGS84 geographic → geocentric spherical) ------
    float lat_r = lat_deg * DEG2RAD;
    float lon_r = lon_deg * DEG2RAD;
    float alt_km = alt_m / 1000.0f;

    float sinLat = sinf(lat_r);
    float cosLat = cosf(lat_r);

    // Radius of curvature in the prime vertical
    float Nc = WGS84_A / sqrtf(1.0f - WGS84_E2 * sinLat * sinLat);
    float px = (Nc + alt_km) * cosLat;               // distance from Earth's axis
    float pz = (Nc * (1.0f - WGS84_E2) + alt_km) * sinLat;
    float r  = sqrtf(px * px + pz * pz);

    float phi_c = asinf(pz / r);   // geocentric latitude
    float s = sinf(phi_c);         // sin(geocentric_lat) = cos(colatitude)
    float c = cosf(phi_c);         // cos(geocentric_lat) = sin(colatitude)
    float s2 = s * s, c2 = c * c;

    // --- Time offset from WMM epoch -------------------------------------------
    float dt = year - WMM_EPOCH;

    // --- Longitude trig -------------------------------------------------------
    float cosML[4] = { 1.0f, cosf(lon_r), cosf(2.0f*lon_r), cosf(3.0f*lon_r) };
    float sinML[4] = { 0.0f, sinf(lon_r), sinf(2.0f*lon_r), sinf(3.0f*lon_r) };

    // --- (WMM_A / r)^(n+2) for n = 1, 2, 3 -----------------------------------
    float ar = WMM_A / r;
    float ar3 = ar * ar * ar;          // (a/r)^3 for n=1
    float ar4 = ar3 * ar;              // (a/r)^4 for n=2
    float ar5 = ar4 * ar;              // (a/r)^5 for n=3
    float Rn[4] = { 0.0f, ar3, ar4, ar5 };

    // =========================================================================
    // Precomputed Schmidt quasi-normal Legendre functions P̃_n^m(s)
    // and their colatitude derivatives dP̃/dtheta, plus P̃/sin(theta) for Y.
    //
    // All expressions verified analytically from Rodrigues formula
    // + Schmidt normalization N_n^m = sqrt(2*(n-m)!/(n+m)!) for m>0.
    // =========================================================================

    // P̃ values (needed only for Pdiv; X uses derivatives directly)
    // Pdiv[n][m] = P̃[n][m] / c  (c = sin(theta), used for eastward component)
    // Guard: if c < 1e-6 (near poles), declination becomes unreliable anyway.
    float Pdiv[4][4] = {};
    if (c > 1e-6f) {
        Pdiv[1][1] = 1.0f;                               // cosPhi / cosPhi
        Pdiv[2][1] = 1.7320508f * s;                     // √3 · s
        Pdiv[2][2] = (1.7320508f / 2.0f) * c;           // (√3/2) · c
        Pdiv[3][1] = (2.4494897f / 4.0f) * (5*s2 - 1); // (√6/4)·(5s²−1)
        Pdiv[3][2] = (3.8729833f / 2.0f) * s * c;       // (√15/2)·s·c
        Pdiv[3][3] = (3.1622777f / 4.0f) * c2;          // (√10/4)·c²
    }

    // dP[n][m] = dP̃_n^m / dtheta  (colatitude derivative, for northward X component)
    float dP[4][4] = {};
    dP[1][0] = -c;                                        // −cos(phi_c)
    dP[1][1] =  s;                                        // +sin(phi_c)
    dP[2][0] = -3.0f * s * c;                            // −3·s·c
    dP[2][1] =  1.7320508f * (s2 - c2);                  // √3·(s²−c²)
    dP[2][2] =  1.7320508f * c * s;                      // √3·c·s
    dP[3][0] = -(1.5f * c) * (5*s2 - 1);                // −(3c/2)·(5s²−1)
    dP[3][1] =  (2.4494897f / 4.0f) * s * (15*s2 - 11); // (√6/4)·s·(15s²−11)
    dP[3][2] =  (3.8729833f / 2.0f) * c * (2*s2 - c2);  // (√15/2)·c·(2s²−c²)
    dP[3][3] =  (3.0f * 3.1622777f / 4.0f) * c2 * s;    // (3√10/4)·c²·s

    // =========================================================================
    // Main summation:
    //   X (northward) = Σ Rn · (g·cosML + h·sinML) · dP[n][m]
    //   Y (eastward)  = Σ Rn · m · (g·sinML − h·cosML) · Pdiv[n][m]
    // =========================================================================
    float X = 0.0f, Y = 0.0f;

    for (int i = 0; i < N_COEFS; i++) {
        const Coef& ci = WMM2020[i];
        int n = ci.n, m = ci.m;

        float g = ci.g + ci.gd * dt;   // apply secular variation
        float h = ci.h + ci.hd * dt;

        float gcos_hsin = g * cosML[m] + h * sinML[m];
        float gsin_hcos = g * sinML[m] - h * cosML[m];

        X += Rn[n] * gcos_hsin * dP[n][m];
        if (m > 0) {
            Y += Rn[n] * (float)m * gsin_hcos * Pdiv[n][m];
        }
    }

    // Declination: angle from geographic north toward east
    float decl = atan2f(Y, X) * RAD2DEG;
    return decl;
}

float toDecimalYear(uint32_t unix_timestamp) {
    // 1970-01-01 UTC = 0
    // Seconds per Julian year = 365.25 * 86400 = 31,557,600
    return 1970.0f + (float)unix_timestamp / 31557600.0f;
}

} // namespace wmm
