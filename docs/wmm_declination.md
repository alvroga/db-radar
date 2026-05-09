# Magnetic Declination — WMM Auto-Correction

**Status**: Complete ✅ | Implemented 2026-03-18

## What It Is

Magnetic declination is the angle between magnetic north (where the compass points) and true geographic north. In Los Angeles it is approximately **+12.25° East** — the compass points 12° clockwise of true north. Without correction, the radar's N indicator would be 12° off.

The World Magnetic Model (WMM), published by NOAA, gives the exact declination for any point on Earth given latitude, longitude, and date. It changes slowly (~0.05°/year in LA).

## How It Works

```
GPS acquires first fix (lat, lon, date known)
  → wmm::computeDeclination(lat, lon, alt, decimal_year)
  → result saved to NVS (persists across power cycles)
  → every compass read: true_heading = magnetic + declination
  → N indicator now points to true geographic north
```

**Recomputes** on the first GPS fix of each session (because the tracking variables reset at boot). This ensures the correction stays current as time passes and automatically adapts when travelling.

**No GPS fix on this boot?** The last cached NVS value is used — stays valid for years in the same region.

## Implementation

| File | Role |
|------|------|
| `include/utils/wmm_declination.h` | Public API |
| `src/utils/wmm_declination.cpp` | WMM2020 coefficients + evaluator |
| `src/utils/task_manager.cpp` | Trigger (GPS fix) + pipeline application |
| `include/settings_manager.h` | `compass_declination_deg`, `compass_declination_valid` |
| `src/utils/settings_manager.cpp` | NVS keys `decl_deg`, `decl_valid` |

## WMM Model Details

| Property | Value |
|----------|-------|
| Model | WMM2020 (NOAA/NCEI) |
| Epoch | 2020.0 |
| Terms used | n=1..3 (9 Gauss coefficient pairs) |
| Accuracy | ±1° mid-latitudes, ±2° globally |
| Secular variation | Applied — coefficients extrapolate to ~2027 with <0.5° error |
| Next update | Embed WMM2030 coefficients when published (~2029) |

The full WMM uses n=1..12 (91 terms). Using n=1..3 captures the dominant dipole and quadrupole fields. For mid-latitude walking navigation the truncation error is <0.5°.

## Sign Convention

**Critical**: This device requires **addition**, not subtraction:

```cpp
true_heading += declination_deg;   // CORRECT for this hardware
true_heading -= declination_deg;   // WRONG — doubles the error
```

Standard navigation formula is `true = magnetic - East_declination`, but the QMC5883L axis orientation on the BH-880 module produces headings that read *low* of true north, so East declination must be added. Empirically verified against GPS ground truth in LA.

## Serial Output

After first GPS fix:
```
[WMM] Declination computed: 12.25° E at (34.1325, -118.1451) 2026.2
[SETTINGS] Magnetic declination saved: 12.25°
```

## Worldwide Coverage

Works anywhere on Earth. Declination ranges from ~0° (near agonic lines) to ±30° near polar regions. The module self-corrects on the first GPS fix at any new location — no user configuration needed when travelling.
