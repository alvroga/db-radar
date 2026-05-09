#pragma once

#include <stdint.h>

namespace beacon_proximity {

// ============================================================================
// Beacon Proximity v2 - "Hot/Cold" Zone Detector
// ============================================================================
// BLE RSSI cannot be a reliable rangefinder (requires UWB for that).
// This redesign treats RSSI as a zone detector with trend awareness:
// - 3 zones instead of 5 distance-based thresholds
// - EMA smoothing (α=0.2) for stable readings
// - Hysteresis (±3 dB) to prevent oscillation
// - Trend detection (approaching/departing) via linear regression
// ============================================================================

/**
 * @brief Proximity zone based on RSSI thresholds
 * Simplified from 5 distance zones to 3 RSSI zones
 */
enum class ProximityZone {
    OUT_OF_RANGE,   // RSSI < -90 dBm: Silent
    VERY_FAR,       // -90 to -85 dBm: 40 BPM  (1500ms) — first detection
    FAR,            // -85 to -75 dBm: 80 BPM  ( 750ms) — getting warmer
    MEDIUM,         // -75 to -65 dBm: 120 BPM ( 500ms) — close
    CLOSE           // RSSI >= -65 dBm: 240 BPM ( 250ms) — on top of it
};

/**
 * @brief Movement trend based on RSSI slope
 * Calculated from linear regression of last 10 EMA samples
 */
enum class MovementTrend {
    UNKNOWN,        // Insufficient data
    STABLE,         // |slope| < 2 dBm/cycle
    APPROACHING,    // slope > +2 dBm/cycle (RSSI increasing)
    DEPARTING       // slope < -2 dBm/cycle (RSSI decreasing)
};

/**
 * @brief Beacon state structure - expanded for v2
 */
struct BeaconState {
    // Core state
    bool scanning_enabled = false;   // Currently scanning for beacon
    bool found = false;              // Beacon detected in last scan

    // RSSI tracking
    int8_t rssi_raw = -127;          // Last raw RSSI reading
    float rssi_ema = -127.0f;        // EMA-smoothed RSSI (α=0.4) — used for zone/trend/labels
    float rssi_display = -127.0f;    // Slow EMA for gauge arc (α=0.25) — analog meter feel

    // Zone management
    ProximityZone zone = ProximityZone::OUT_OF_RANGE;           // Current confirmed zone
    ProximityZone pending_zone = ProximityZone::OUT_OF_RANGE;   // Zone being validated
    int pending_zone_count = 0;                                  // Consecutive samples in pending zone

    // Trend detection
    MovementTrend trend = MovementTrend::UNKNOWN;   // Current movement trend

    // Timing
    uint32_t last_seen_ms = 0;       // Millis of last detection
    uint32_t cycle_start_ms = 0;     // 5s cycle timing for sonar

    // Legacy compatibility (for display only)
    float distance_m = 99.9f;        // Estimated distance in meters
};

/**
 * @brief Initialize beacon proximity module
 * Must be called after BLE is initialized
 */
void init();

/**
 * @brief Deinitialize NimBLE stack to free ~25KB SRAM for WiFi.
 * WiFi and BLE cannot coexist — call before wifi_manager::init().
 * After deinit, beacon proximity won't work until reboot.
 */
void deinit();

/**
 * @brief Set beacon proximity scanning enabled/disabled
 * Called when entering/leaving 50m zoom
 * @param enabled True to start scanning, false to stop
 */
void setEnabled(bool enabled);

/**
 * @brief Check if beacon scanning is currently active
 */
bool isEnabled();

/**
 * @brief Update beacon proximity state
 * Call this from the Network Task loop
 * Handles scanning, RSSI averaging, and distance calculation
 */
void update();

/**
 * @brief Update sonar beeping (call frequently for steady rhythm)
 * Call this from a fast loop (UI Task, every 15-50ms)
 * Handles only the ping timing - lightweight, no BLE operations
 */
void updateSonar();

/**
 * @brief Get current beacon state
 */
BeaconState getState();

/**
 * @brief Get current proximity zone
 */
ProximityZone getCurrentZone();

/**
 * @brief Get current movement trend
 */
MovementTrend getCurrentTrend();

/**
 * @brief Convert zone to string
 */
const char* zoneToString(ProximityZone zone);

/**
 * @brief Convert trend to string
 */
const char* trendToString(MovementTrend trend);

/**
 * @brief Get estimated distance to beacon
 * @return Distance in meters, or 99.9 if beacon not found
 */
float getDistance();

/**
 * @brief Check if beacon is within a threshold distance
 * @param threshold_m Distance threshold in meters
 * @return True if beacon found and within threshold
 */
bool isBeaconNearby(float threshold_m = 10.0f);

/**
 * @brief Calculate distance from RSSI
 * Uses formula: distance = 10^((measuredPower - RSSI) / (10 * n))
 * @param rssi Current RSSI reading (dBm)
 * @param measured_power Calibrated RSSI at 1 meter (dBm)
 * @param n Path loss exponent (2.0=open space, 4.0=indoor)
 * @return Estimated distance in meters
 */
float rssiToDistance(int8_t rssi, int8_t measured_power, float n);

/**
 * @brief Get beep interval based on distance
 * @param distance_m Distance in meters
 * @return Interval in milliseconds (0 = no beep)
 */
uint16_t getBeepInterval(float distance_m);

/**
 * @brief Debug: Scan and list all visible BLE devices
 * Blocking call that scans for 3 seconds and prints all found devices
 */
void debugScanAll();

/**
 * @brief Debug: Print current module state
 */
void debugPrintState();

/**
 * @brief Reset all smoothing and trend state
 * Used when re-enabling scanning or for debug purposes
 */
void resetState();

/**
 * @brief Suppress beacon sonar updates (waypoint fix takes priority)
 * When suppressed, updateSonar() does nothing so the caller can drive the buzzer.
 * @param suppress True to suppress, false to resume
 */
void suppressSonar(bool suppress);

/**
 * @brief Mark beacon as found / un-found (NVS-persisted, survives power cycle)
 * setFound(true)  → suppresses sonar, clears visual feedback
 * setFound(false) → resumes sonar (tap in Settings Beacon tab to reset)
 */
void setFound(bool found);

/**
 * @brief Check if beacon has been marked as found this session
 */
bool isFound();

} // namespace beacon_proximity
