#include "navigation/gps_quality.h"
#include <math.h>

#ifndef DEG_TO_RAD
#define DEG_TO_RAD (M_PI / 180.0)
#endif

namespace gps_quality {

// Internal state
static QualityMetrics g_metrics;
static GPSData g_last_gps_data;
static uint32_t g_init_time_ms = 0;
static uint32_t g_first_fix_time_ms = 0;
static bool g_first_fix_recorded = false;

// Haversine distance calculation (for jump detection)
static double haversine(double lat1, double lon1, double lat2, double lon2) {
    const double R = 6371000.0; // Earth radius in meters
    double dLat = (lat2 - lat1) * DEG_TO_RAD;
    double dLon = (lon2 - lon1) * DEG_TO_RAD;
    double a = sin(dLat / 2.0) * sin(dLat / 2.0) +
               cos(lat1 * DEG_TO_RAD) * cos(lat2 * DEG_TO_RAD) *
               sin(dLon / 2.0) * sin(dLon / 2.0);
    double c = 2.0 * atan2(sqrt(a), sqrt(1.0 - a));
    return R * c;
}

void init() {
    g_metrics = QualityMetrics();
    g_last_gps_data = GPSData();
    g_init_time_ms = millis();
    g_first_fix_time_ms = 0;
    g_first_fix_recorded = false;
    Serial.println("[GPS_QUALITY] Quality tracking initialized");
}

float calculateQualityScore(const GPSData& gps_data) {
    if (!gps_data.valid) {
        return 0.0f; // No fix = 0%
    }

    float score = 0.0f;

    // Factor 1: HDOP (50% weight)
    // HDOP interpretation:
    // < 1.0 = Excellent (100%)
    // 1.0-2.0 = Good (80%)
    // 2.0-5.0 = Moderate (50%)
    // 5.0-10.0 = Fair (25%)
    // > 10.0 = Poor (10%)
    float hdop_score = 0.0f;
    if (!isnan(gps_data.hdop)) {
        if (gps_data.hdop < 1.0f) {
            hdop_score = 100.0f;
        } else if (gps_data.hdop < 2.0f) {
            hdop_score = 80.0f;
        } else if (gps_data.hdop < 5.0f) {
            hdop_score = 50.0f - (gps_data.hdop - 2.0f) * 10.0f; // Linear 50-20%
        } else if (gps_data.hdop < 10.0f) {
            hdop_score = 25.0f - (gps_data.hdop - 5.0f) * 3.0f; // Linear 25-10%
        } else {
            hdop_score = 10.0f;
        }
    } else {
        hdop_score = 50.0f; // Unknown HDOP = assume moderate
    }
    score += hdop_score * 0.5f; // 50% weight

    // Factor 2: Satellite Count (30% weight)
    // < 4 satellites = unstable (0%)
    // 4-5 satellites = minimum (40%)
    // 6-9 satellites = good (70%)
    // 10+ satellites = excellent (100%)
    float sat_score = 0.0f;
    if (gps_data.sats < 4) {
        sat_score = 0.0f;
    } else if (gps_data.sats < 6) {
        sat_score = 40.0f;
    } else if (gps_data.sats < 10) {
        sat_score = 40.0f + (gps_data.sats - 6) * 10.0f; // Linear 40-100%
    } else {
        sat_score = 100.0f;
    }
    score += sat_score * 0.3f; // 30% weight

    // Factor 3: Position Stability (20% weight)
    // Recent position jump = 0%
    // No recent jump = 100%
    float stability_score = gps_data.position_jump_detected ? 0.0f : 100.0f;
    score += stability_score * 0.2f; // 20% weight

    // Store component scores for diagnostics
    g_metrics.hdop_score = hdop_score;
    g_metrics.satellite_score = sat_score;
    g_metrics.stability_score = stability_score;

    return constrain(score, 0.0f, 100.0f);
}

bool detectPositionJump(const GPSData& current, const GPSData& previous) {
    // Skip if either position is invalid
    if (!current.valid || !previous.valid) {
        return false;
    }

    // Skip if this is the first fix
    if (previous.lat == 0.0 && previous.lon == 0.0) {
        return false;
    }

    // Calculate distance between consecutive fixes
    double distance_m = haversine(previous.lat, previous.lon, current.lat, current.lon);

    // Calculate time delta
    uint32_t time_delta_ms = current.last_update_ms - previous.last_update_ms;
    if (time_delta_ms == 0) time_delta_ms = 200; // Assume 5Hz (200ms) if unknown

    // Calculate maximum plausible distance based on speed
    // Conservative estimate: 120 km/h (33 m/s) max speed
    float max_plausible_m = 33.0f * (time_delta_ms / 1000.0f) * 2.0f; // 2× safety margin

    // Add GPS accuracy margin (assume ±5m typical error)
    max_plausible_m += 10.0f;

    // Detect jump
    if (distance_m > max_plausible_m) {
        Serial.printf("[GPS_QUALITY] ⚠ Position jump detected: %.1fm in %dms (max plausible: %.1fm)\n",
                      distance_m, time_delta_ms, max_plausible_m);
        g_metrics.position_jumps_detected++;
        return true;
    }

    return false;
}

QualityMetrics update(const GPSData& gps_data) {
    // Update timestamp
    g_metrics.time_since_last_update_ms = millis() - gps_data.last_update_ms;

    // Track first fix time
    if (gps_data.valid && !g_first_fix_recorded) {
        g_first_fix_time_ms = millis() - g_init_time_ms;
        g_first_fix_recorded = true;
        Serial.printf("[GPS_QUALITY] ✓ First fix acquired in %d seconds\n", g_first_fix_time_ms / 1000);
    }

    // Detect position jumps
    bool jump_detected = false;
    if (g_last_gps_data.valid && gps_data.valid) {
        jump_detected = detectPositionJump(gps_data, g_last_gps_data);
    }

    // Store jump detection in local copy for quality calculation
    GPSData gps_data_copy = gps_data;
    gps_data_copy.position_jump_detected = jump_detected;

    // Calculate overall quality score
    g_metrics.overall_score = calculateQualityScore(gps_data_copy);

    // Update health indicators
    g_metrics.fix_available = gps_data.valid;
    g_metrics.good_accuracy = !isnan(gps_data.hdop) && gps_data.hdop < 2.0f;
    g_metrics.excellent_accuracy = !isnan(gps_data.hdop) && gps_data.hdop < 1.0f;
    g_metrics.enough_satellites = gps_data.sats >= 6;
    g_metrics.position_stable = !gps_data.position_jump_detected;

    // Update statistics
    g_metrics.updates_received++;

    // Store for next comparison
    g_last_gps_data = gps_data;

    return g_metrics;
}

const QualityMetrics& getMetrics() {
    return g_metrics;
}

const char* getQualityDescription(float quality_score) {
    if (quality_score >= 80.0f) return "Excellent";
    if (quality_score >= 60.0f) return "Good";
    if (quality_score >= 40.0f) return "Fair";
    if (quality_score >= 20.0f) return "Poor";
    return "No Fix";
}

const char* getHDOPDescription(float hdop) {
    if (isnan(hdop)) return "Unknown";
    if (hdop < 1.0f) return "Excellent";
    if (hdop < 2.0f) return "Good";
    if (hdop < 5.0f) return "Moderate";
    if (hdop < 10.0f) return "Fair";
    return "Poor";
}

void printReport() {
    Serial.println("\n=== GPS Quality Report ===");
    Serial.printf("Overall Quality: %.1f%% (%s)\n",
                  g_metrics.overall_score,
                  getQualityDescription(g_metrics.overall_score));
    Serial.println();

    // Component scores
    Serial.println("Component Scores:");
    Serial.printf("  HDOP:       %.1f%% (%s)\n", g_metrics.hdop_score,
                  getHDOPDescription(g_last_gps_data.hdop));
    Serial.printf("  Satellites: %.1f%% (%d sats)\n", g_metrics.satellite_score, g_last_gps_data.sats);
    Serial.printf("  Stability:  %.1f%% (%s)\n", g_metrics.stability_score,
                  g_metrics.position_stable ? "stable" : "unstable");
    Serial.println();

    // Health indicators
    Serial.println("Health Status:");
    Serial.printf("  Fix Available:      %s\n", g_metrics.fix_available ? "YES" : "NO");
    Serial.printf("  Good Accuracy:      %s (HDOP < 2.0)\n", g_metrics.good_accuracy ? "YES" : "NO");
    Serial.printf("  Excellent Accuracy: %s (HDOP < 1.0)\n", g_metrics.excellent_accuracy ? "YES" : "NO");
    Serial.printf("  Enough Satellites:  %s (>= 6)\n", g_metrics.enough_satellites ? "YES" : "NO");
    Serial.printf("  Position Stable:    %s\n", g_metrics.position_stable ? "YES" : "NO");
    Serial.println();

    // Statistics
    Serial.println("Statistics:");
    Serial.printf("  Updates Received:   %d\n", g_metrics.updates_received);
    Serial.printf("  Position Jumps:     %d\n", g_metrics.position_jumps_detected);
    Serial.printf("  Time Since Update:  %d ms\n", g_metrics.time_since_last_update_ms);
    if (g_first_fix_recorded) {
        Serial.printf("  Time to First Fix:  %d seconds\n", g_first_fix_time_ms / 1000);
    } else {
        Serial.println("  Time to First Fix:  (no fix yet)");
    }
    Serial.println("=========================\n");
}

void reset() {
    init();
    Serial.println("[GPS_QUALITY] Quality tracking reset");
}

} // namespace gps_quality
