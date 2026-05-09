#pragma once
#include "core/arduino_compat.h"
#include "gps_bh880.h"

namespace gps_quality {

// GPS Quality Assessment
// ======================
// Calculates GPS signal quality score (0-100%) based on multiple factors

struct QualityMetrics {
    float overall_score = 0.0f;      // 0-100% (composite quality)
    float hdop_score = 0.0f;         // 0-100% (based on HDOP)
    float satellite_score = 0.0f;    // 0-100% (based on sat count)
    float stability_score = 0.0f;    // 0-100% (position stability)

    // Signal health indicators
    bool fix_available = false;
    bool good_accuracy = false;      // HDOP < 2.0 (good)
    bool excellent_accuracy = false; // HDOP < 1.0 (excellent)
    bool enough_satellites = false;  // >= 6 satellites
    bool position_stable = false;    // No recent jumps

    // Performance stats
    uint32_t updates_received = 0;
    uint32_t position_jumps_detected = 0;
    uint32_t time_since_last_update_ms = 0;
};

// Initialize GPS quality tracking
void init();

// Update quality metrics with new GPS data
// Returns: updated quality metrics
QualityMetrics update(const GPSData& gps_data);

// Get current quality metrics
const QualityMetrics& getMetrics();

// Calculate quality score (0-100%) from GPS data
// Factors: HDOP, satellite count, position stability
float calculateQualityScore(const GPSData& gps_data);

// Detect suspicious position jumps (GPS glitches)
// Returns: true if jump detected (position changed > max_plausible_distance)
bool detectPositionJump(const GPSData& current, const GPSData& previous);

// Get human-readable quality description
const char* getQualityDescription(float quality_score);

// Get HDOP quality description
const char* getHDOPDescription(float hdop);

// Print detailed GPS quality report to serial
void printReport();

// Reset quality tracking (for testing)
void reset();

} // namespace gps_quality
