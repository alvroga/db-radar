#include "hardware/connectivity/beacon_proximity.h"
#include "settings_manager.h"
#include "hardware/buzzer.h"
#include "core/arduino_compat.h"
#include <time.h>          // NimBLEAttValue.h uses time_t without including this itself
#include <NimBLEDevice.h>
#include <cmath>

namespace beacon_proximity {

// ============================================================================
// Configuration Constants - v2 "Hot/Cold" Detector
// ============================================================================

// Timing
static constexpr uint32_t SCAN_DURATION_SEC = 1;      // 1s scan (was 2s — shorter = faster zone updates)
static constexpr uint32_t SCAN_INTERVAL_MS = 500;     // Scan every 500ms — faster RSSI updates when beacon in range

// EMA smoothing
static constexpr float EMA_ALPHA = 0.4f;              // Moderate smoothing (~5 sample response)
static constexpr float DISPLAY_EMA_ALPHA = 0.25f;    // Slow EMA for gauge arc (~8 scans to 90% settle)
static constexpr int TREND_HISTORY_SIZE = 10;         // Samples for trend calculation

// Zone thresholds (RSSI-based, not distance-based)
static constexpr int8_t ZONE_CLOSE_THRESHOLD    = -65;  // RSSI >= -65 = CLOSE
static constexpr int8_t ZONE_MEDIUM_THRESHOLD   = -75;  // RSSI >= -75 = MEDIUM
static constexpr int8_t ZONE_FAR_THRESHOLD      = -85;  // RSSI >= -85 = FAR
static constexpr int8_t ZONE_VERY_FAR_THRESHOLD = -90;  // RSSI >= -90 = VERY_FAR
static constexpr int8_t HYSTERESIS_DB = 3;              // ±3 dB band
static constexpr int ZONE_CHANGE_SAMPLES = 2;         // 2 consecutive readings to confirm zone change

// Trend detection
static constexpr float TREND_APPROACHING_THRESHOLD = 2.0f;   // dBm/cycle positive
static constexpr float TREND_DEPARTING_THRESHOLD = -2.0f;    // dBm/cycle negative

// Lost beacon timeout
static constexpr uint32_t BEACON_LOST_TIMEOUT_MS = 15000;  // 15 seconds

// ============================================================================
// Module State
// ============================================================================

static BeaconState g_state;
static bool g_initialized = false;
static bool g_sonar_suppressed = false;  // True when waypoint fix has sonar priority
static bool g_found = false;             // True when user has tapped the ball (in-session only)
static uint32_t g_last_scan_ms = 0;
static NimBLEScan* g_pScan = nullptr;
static bool g_scan_in_progress = false;

// Target beacon (lowercase for comparison)
static char g_target_mac_lower[18] = "";

// Spinlock protecting g_state fields written by NimBLE callback and read by Network Task
static portMUX_TYPE g_state_mux = portMUX_INITIALIZER_UNLOCKED;

// Trend history circular buffer
static float g_trend_history[TREND_HISTORY_SIZE];
static int g_trend_index = 0;
static int g_trend_count = 0;  // How many samples in history


// ============================================================================
// Forward Declarations
// ============================================================================

static void updateRSSI_EMA(int8_t raw);
static void calculateTrend();
static void updateZone(float ema, bool& zone_changed_out, ProximityZone& changed_to_out);
static ProximityZone classifyRSSI(float rssi, ProximityZone current_zone);

// ============================================================================
// BLE Scan Callback
// ============================================================================

class BeaconScanCallback : public NimBLEAdvertisedDeviceCallbacks {
    void onResult(NimBLEAdvertisedDevice* advertisedDevice) override {
        String mac = advertisedDevice->getAddress().toString().c_str();
        mac = mac.toLowerCase();

        if (mac == g_target_mac_lower) {
            int rssi = advertisedDevice->getRSSI();

            portENTER_CRITICAL(&g_state_mux);
            g_state.rssi_raw = (int8_t)rssi;
            g_state.found = true;
            g_state.last_seen_ms = millis();
            updateRSSI_EMA((int8_t)rssi);
            portEXIT_CRITICAL(&g_state_mux);

            // Stop scanning early (outside spinlock — BLE API call)
            g_pScan->stop();
        }
    }
};

static BeaconScanCallback g_scanCallback;

// ============================================================================
// EMA Smoothing
// ============================================================================

static void updateRSSI_EMA(int8_t raw) {
    if (g_state.rssi_ema <= -127.0f) {
        // First reading - initialize both EMAs
        g_state.rssi_ema = (float)raw;
        g_state.rssi_display = (float)raw;
    } else {
        // EMA formula: ema = α*raw + (1-α)*ema
        g_state.rssi_ema = EMA_ALPHA * (float)raw + (1.0f - EMA_ALPHA) * g_state.rssi_ema;
        // Display EMA: second-stage slow smoothing on rssi_ema for the gauge arc
        g_state.rssi_display = DISPLAY_EMA_ALPHA * g_state.rssi_ema
                             + (1.0f - DISPLAY_EMA_ALPHA) * g_state.rssi_display;
    }

    // Add to trend history
    g_trend_history[g_trend_index] = g_state.rssi_ema;
    g_trend_index = (g_trend_index + 1) % TREND_HISTORY_SIZE;
    if (g_trend_count < TREND_HISTORY_SIZE) {
        g_trend_count++;
    }
}

// ============================================================================
// Trend Detection (Linear Regression)
// ============================================================================

static void calculateTrend() {
    if (g_trend_count < 3) {
        g_state.trend = MovementTrend::UNKNOWN;
        return;
    }

    // Calculate linear regression slope
    // Using least squares: slope = Σ((x-x̄)(y-ȳ)) / Σ((x-x̄)²)
    float sum_x = 0, sum_y = 0;
    int n = g_trend_count;

    // Calculate means
    for (int i = 0; i < n; i++) {
        int idx = (g_trend_index - n + i + TREND_HISTORY_SIZE) % TREND_HISTORY_SIZE;
        sum_x += i;
        sum_y += g_trend_history[idx];
    }
    float mean_x = sum_x / n;
    float mean_y = sum_y / n;

    // Calculate slope
    float numerator = 0, denominator = 0;
    for (int i = 0; i < n; i++) {
        int idx = (g_trend_index - n + i + TREND_HISTORY_SIZE) % TREND_HISTORY_SIZE;
        float x_diff = i - mean_x;
        float y_diff = g_trend_history[idx] - mean_y;
        numerator += x_diff * y_diff;
        denominator += x_diff * x_diff;
    }

    if (denominator < 0.001f) {
        g_state.trend = MovementTrend::STABLE;
        return;
    }

    float slope = numerator / denominator;

    // Classify trend
    // Note: Positive slope = RSSI increasing = getting closer
    if (slope > TREND_APPROACHING_THRESHOLD) {
        g_state.trend = MovementTrend::APPROACHING;
    } else if (slope < TREND_DEPARTING_THRESHOLD) {
        g_state.trend = MovementTrend::DEPARTING;
    } else {
        g_state.trend = MovementTrend::STABLE;
    }
}

// ============================================================================
// Zone Classification with Hysteresis
// ============================================================================

static ProximityZone classifyRSSI(float rssi, ProximityZone current_zone) {
    int8_t close_threshold    = ZONE_CLOSE_THRESHOLD;    // -65
    int8_t medium_threshold   = ZONE_MEDIUM_THRESHOLD;   // -75
    int8_t far_threshold      = ZONE_FAR_THRESHOLD;      // -85
    int8_t very_far_threshold = ZONE_VERY_FAR_THRESHOLD; // -90

    // Apply hysteresis per boundary based on current zone.
    // Raise the exit bar to prevent rapid oscillation at zone edges.
    switch (current_zone) {
        case ProximityZone::CLOSE:
            close_threshold    -= HYSTERESIS_DB;  // Must drop to -68 to leave CLOSE
            break;
        case ProximityZone::MEDIUM:
            close_threshold    += HYSTERESIS_DB;  // Must reach -62 to enter CLOSE
            medium_threshold   -= HYSTERESIS_DB;  // Must drop to -78 to leave MEDIUM
            break;
        case ProximityZone::FAR:
            medium_threshold   += HYSTERESIS_DB;  // Must reach -72 to enter MEDIUM
            far_threshold      -= HYSTERESIS_DB;  // Must drop to -88 to leave FAR
            break;
        case ProximityZone::VERY_FAR:
            far_threshold      += HYSTERESIS_DB;  // Must reach -82 to enter FAR
            very_far_threshold -= HYSTERESIS_DB;  // Must drop to -93 to leave VERY_FAR
            break;
        case ProximityZone::OUT_OF_RANGE:
            very_far_threshold += HYSTERESIS_DB;  // Must reach -87 to enter VERY_FAR
            break;
    }

    if (rssi >= close_threshold)    return ProximityZone::CLOSE;
    if (rssi >= medium_threshold)   return ProximityZone::MEDIUM;
    if (rssi >= far_threshold)      return ProximityZone::FAR;
    if (rssi >= very_far_threshold) return ProximityZone::VERY_FAR;
    return ProximityZone::OUT_OF_RANGE;
}

static void updateZone(float ema, bool& zone_changed_out, ProximityZone& changed_to_out) {
    // Classify what zone the current EMA would put us in
    ProximityZone new_zone = classifyRSSI(ema, g_state.zone);
    zone_changed_out = false;

    if (new_zone == g_state.zone) {
        // Same zone - reset pending
        g_state.pending_zone = g_state.zone;
        g_state.pending_zone_count = 0;
    } else if (new_zone == g_state.pending_zone) {
        // Consecutive reading in pending zone
        g_state.pending_zone_count++;

        // Require ZONE_CHANGE_SAMPLES consecutive readings to confirm zone change
        if (g_state.pending_zone_count >= ZONE_CHANGE_SAMPLES) {
            g_state.zone = new_zone;
            g_state.pending_zone_count = 0;
            zone_changed_out = true;
            changed_to_out = new_zone;
            // Note: Serial.printf intentionally NOT called here — caller logs outside spinlock
        }
    } else {
        // New pending zone
        g_state.pending_zone = new_zone;
        g_state.pending_zone_count = 1;
    }
}

// ============================================================================
// Public API Implementation
// ============================================================================

void init() {
    if (g_initialized) return;

    Serial.println("[BEACON] Initializing beacon proximity (NimBLE)...");

    // NimBLE stack needs ~25KB of internal SRAM. Check before attempting —
    // partial init failure at very low heap corrupts memory and hangs the UI task.
    // Log free SRAM for diagnostics — init is called at boot before WiFi/tasks consume SRAM
    size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    Serial.printf("[BEACON] Free internal SRAM before BLE init: %u bytes\n", (unsigned)free_internal);

    if (!NimBLEDevice::getInitialized()) {
        NimBLEDevice::init("");
    }

    // Verify BLE actually initialized — don't proceed on partial failure
    if (!NimBLEDevice::getInitialized()) {
        Serial.println("[BEACON] ERROR: NimBLE init failed - beacon proximity disabled");
        return;
    }

    g_pScan = NimBLEDevice::getScan();
    if (!g_pScan) {
        Serial.println("[BEACON] ERROR: Failed to get NimBLE scan object - beacon proximity disabled");
        return;
    }

    g_pScan->setAdvertisedDeviceCallbacks(&g_scanCallback, false);
    g_pScan->setActiveScan(true);
    g_pScan->setInterval(100);
    g_pScan->setWindow(80);

    g_initialized = true;

    // Restore persisted found state
    g_found = settings_manager::getSettings().beacon_found;
    if (g_found) suppressSonar(true);

    Serial.println("[BEACON] Beacon proximity initialized (NimBLE — ~25KB SRAM)");
    Serial.printf("[BEACON] Found state restored from NVS: %s\n", g_found ? "FOUND" : "MISSING");
    Serial.println("[BEACON] Zones: OUT_OF_RANGE < -85dBm < FAR < -65dBm < CLOSE");
}

void deinit() {
    if (!g_initialized) return;

    setEnabled(false);  // Stop any ongoing BLE scan first

    if (NimBLEDevice::getInitialized()) {
        g_pScan = nullptr;
        NimBLEDevice::deinit(true);
    }

    g_initialized = false;
    Serial.println("[BEACON] NimBLE deinitialized — ~25KB SRAM freed for WiFi");
}

void setEnabled(bool enabled) {
    if (!g_initialized) {
        init();
    }

    // If init failed (no BLE memory), silently skip — don't set scanning_enabled
    if (!g_initialized) {
        return;
    }

    if (enabled && !g_state.scanning_enabled) {
        const auto& settings = settings_manager::getSettings();

        if (settings.beacon_count == 0 || strlen(settings.beacon_macs[0]) == 0) {
            Serial.println("[BEACON] No beacon MAC configured - proximity disabled");
            return;
        }

        // Convert first beacon MAC to lowercase for comparison
        strncpy(g_target_mac_lower, settings.beacon_macs[0], sizeof(g_target_mac_lower) - 1);
        g_target_mac_lower[sizeof(g_target_mac_lower) - 1] = '\0';
        for (int i = 0; g_target_mac_lower[i]; i++) {
            g_target_mac_lower[i] = tolower((unsigned char)g_target_mac_lower[i]);
        }

        // Reset all state
        resetState();

        g_state.scanning_enabled = true;
        Serial.printf("[BEACON] Proximity scanning ENABLED for %s\n", settings.beacon_macs[0]);
        Serial.println("[BEACON] 5s cycle: 2s scan → 3s beep presentation");

    } else if (!enabled && g_state.scanning_enabled) {
        g_state.scanning_enabled = false;
        g_state.found = false;
        buzzer::stopSonar();  // Stop immediately — don't wait for Network Task's updateSonar()
        Serial.println("[BEACON] Proximity scanning DISABLED");
    }
}

bool isEnabled() {
    return g_state.scanning_enabled;
}

void resetState() {
    // Reset RSSI tracking
    g_state.rssi_raw = -127;
    g_state.rssi_ema = -127.0f;
    g_state.rssi_display = -127.0f;
    g_state.found = false;

    // Reset zone state
    g_state.zone = ProximityZone::OUT_OF_RANGE;
    g_state.pending_zone = ProximityZone::OUT_OF_RANGE;
    g_state.pending_zone_count = 0;

    // Reset trend state
    g_state.trend = MovementTrend::UNKNOWN;
    g_trend_index = 0;
    g_trend_count = 0;
    for (int i = 0; i < TREND_HISTORY_SIZE; i++) {
        g_trend_history[i] = -127.0f;
    }

    // Reset timing
    g_state.last_seen_ms = 0;
    g_state.cycle_start_ms = 0;
    g_last_scan_ms = 0;

    // Reset sonar state
    buzzer::stopSonar();

    // Reset legacy
    g_state.distance_m = 99.9f;

    Serial.println("[BEACON] State reset complete");
}

void update() {
    if (!g_initialized || !g_state.scanning_enabled) {
        return;
    }

    uint32_t now = millis();

    // Check if beacon was lost (no reading for 15s)
    bool beacon_lost = false;
    portENTER_CRITICAL(&g_state_mux);
    if (g_state.last_seen_ms > 0 && (now - g_state.last_seen_ms) > BEACON_LOST_TIMEOUT_MS) {
        if (g_state.zone != ProximityZone::OUT_OF_RANGE || g_state.rssi_ema > -127.0f) {
            beacon_lost = true;
            g_state.zone = ProximityZone::OUT_OF_RANGE;
            g_state.pending_zone = ProximityZone::OUT_OF_RANGE;
            g_state.pending_zone_count = 0;
            g_state.trend = MovementTrend::UNKNOWN;
            g_trend_count = 0;
            g_state.rssi_ema = -127.0f;
            g_state.rssi_display = -127.0f;
            g_state.rssi_raw = -127;
        }
    }
    portEXIT_CRITICAL(&g_state_mux);
    if (beacon_lost) {
        Serial.println("[BEACON] Beacon lost - resetting to OUT_OF_RANGE");
    }

    // Check if previous scan completed
    if (g_scan_in_progress && !g_pScan->isScanning()) {
        g_scan_in_progress = false;

        // Process scan results (handles case where beacon wasn't caught by callback)
        if (g_pScan) {
            NimBLEScanResults results = g_pScan->getResults();
            int count = results.getCount();

            for (int i = 0; i < count; i++) {
                NimBLEAdvertisedDevice device = results.getDevice(i);
                String mac = device.getAddress().toString().c_str();
                mac = mac.toLowerCase();

                if (mac == g_target_mac_lower) {
                    int rssi = device.getRSSI();
                    portENTER_CRITICAL(&g_state_mux);
                    g_state.rssi_raw = (int8_t)rssi;
                    g_state.found = true;
                    g_state.last_seen_ms = millis();
                    updateRSSI_EMA((int8_t)rssi);
                    portEXIT_CRITICAL(&g_state_mux);
                    break;
                }
            }
        }

        // After getting new RSSI, update zone and trend.
        // Hold spinlock to keep g_state consistent — NimBLE may deliver a late onResult()
        // callback after isScanning() returns false. Serial.printf is intentionally outside
        // the critical section (Serial uses interrupts).
        bool zone_changed = false;
        ProximityZone zone_changed_to = ProximityZone::OUT_OF_RANGE;
        portENTER_CRITICAL(&g_state_mux);
        if (g_state.rssi_ema > -127.0f) {
            updateZone(g_state.rssi_ema, zone_changed, zone_changed_to);
            calculateTrend();
            const auto& settings = settings_manager::getSettings();
            g_state.distance_m = rssiToDistance(
                (int8_t)g_state.rssi_ema,
                settings.beacon_measured_power,
                settings.beacon_path_loss_n
            );
        }
        portEXIT_CRITICAL(&g_state_mux);
        if (zone_changed) {
            Serial.printf("[BEACON] Zone changed: %s\n", zoneToString(zone_changed_to));
        }

    }

    // Time for a new scan?
    if (!g_scan_in_progress && (now - g_last_scan_ms >= SCAN_INTERVAL_MS)) {
        g_last_scan_ms = now;
        g_scan_in_progress = true;
        g_state.found = false;

        if (g_pScan) {
            g_pScan->clearResults();
            g_pScan->start(SCAN_DURATION_SEC, nullptr, false);  // Non-blocking
        }
    }
}

void suppressSonar(bool suppress) {
    g_sonar_suppressed = suppress;
}

void setFound(bool found) {
    g_found = found;
    suppressSonar(found);  // silence sonar immediately when found; resume when un-found
    settings_manager::saveBeaconFound(found);
    Serial.printf("[BEACON] Beacon marked %s\n", found ? "FOUND" : "MISSING");
}

bool isFound() {
    return g_found;
}

void updateSonar() {
    if (!g_state.scanning_enabled) { buzzer::stopSonar(); return; }

    // Found beacon — no audio (NVS-persisted, user must manually clear)
    if (g_found) { buzzer::stopSonar(); return; }

    // Waypoint fix has sonar priority — do not overwrite its interval
    if (g_sonar_suppressed) return;

    // Beacon must actually be in range (zone check prevents stale rssi_ema from beeping)
    if (g_state.zone == ProximityZone::OUT_OF_RANGE) { buzzer::stopSonar(); return; }

    const auto& settings = settings_manager::getSettings();
    // Respect both master sound toggle and beacon-specific sound toggle
    if (!settings.button_sound_enabled || !settings.beacon_sound_enabled) {
        buzzer::stopSonar();
        return;
    }

    // Use confirmed zone for interval — zone has ±3 dBm hysteresis + 2 consecutive
    // readings required, so tempo stays metronomic within a zone and only steps
    // when you've genuinely crossed a boundary. Musical BPM progression:
    //   VERY_FAR: 1500ms = 40 BPM  (andante)   — first detection, slow pulse
    //   FAR:       750ms = 80 BPM  (moderato)   — getting warmer
    //   MEDIUM:    500ms = 120 BPM (allegro)    — close
    //   CLOSE:     250ms = 240 BPM (prestissimo) — on top of it
    uint32_t interval_ms = 0;
    switch (g_state.zone) {
        case ProximityZone::CLOSE:    interval_ms = 250;  break;
        case ProximityZone::MEDIUM:   interval_ms = 500;  break;
        case ProximityZone::FAR:      interval_ms = 750;  break;
        case ProximityZone::VERY_FAR: interval_ms = 1500; break;
        default: buzzer::stopSonar(); return;
    }

    buzzer::setSonarInterval(interval_ms, 30);
}

BeaconState getState() {
    return g_state;
}

ProximityZone getCurrentZone() {
    return g_state.zone;
}

MovementTrend getCurrentTrend() {
    return g_state.trend;
}

const char* zoneToString(ProximityZone zone) {
    switch (zone) {
        case ProximityZone::OUT_OF_RANGE: return "OUT_OF_RANGE";
        case ProximityZone::VERY_FAR:     return "VERY_FAR";
        case ProximityZone::FAR:          return "FAR";
        case ProximityZone::MEDIUM:       return "MEDIUM";
        case ProximityZone::CLOSE:        return "CLOSE";
        default:                          return "UNKNOWN";
    }
}

const char* trendToString(MovementTrend trend) {
    switch (trend) {
        case MovementTrend::UNKNOWN:     return "UNKNOWN";
        case MovementTrend::STABLE:      return "STABLE";
        case MovementTrend::APPROACHING: return "APPROACHING";
        case MovementTrend::DEPARTING:   return "DEPARTING";
        default:                         return "???";
    }
}

float getDistance() {
    return g_state.distance_m;
}

bool isBeaconNearby(float threshold_m) {
    return g_state.found && g_state.distance_m <= threshold_m;
}

float rssiToDistance(int8_t rssi, int8_t measured_power, float n) {
    if (rssi == -127 || rssi >= 0) {
        return 99.9f;
    }

    float exponent = (float)(measured_power - rssi) / (10.0f * n);
    float distance = powf(10.0f, exponent);

    if (distance < 0.1f) distance = 0.1f;
    if (distance > 99.9f) distance = 99.9f;

    return distance;
}

uint16_t getBeepInterval(float distance_m) {
    // Legacy function - kept for compatibility
    if (distance_m > 10.0f) return 0;
    if (distance_m > 5.0f)  return 2000;
    if (distance_m > 3.0f)  return 1000;
    if (distance_m > 1.0f)  return 500;
    return 200;
}

void debugScanAll() {
    Serial.println("=== BLE DEBUG SCAN (NimBLE) ===");
    Serial.println("Scanning for ALL BLE devices (3 seconds)...");

    if (!g_initialized) {
        init();
    }

    if (!g_pScan) {
        Serial.println("[BLE] ERROR: BLE scan object is null!");
        return;
    }

    g_pScan->setAdvertisedDeviceCallbacks(nullptr, false);
    g_pScan->setActiveScan(true);
    g_pScan->clearResults();

    NimBLEScanResults foundDevices = g_pScan->start(3, false);  // Blocking scan

    int count = foundDevices.getCount();
    Serial.printf("[BLE] Found %d devices:\n", count);
    Serial.println("----------------------------------------");

    for (int i = 0; i < count; i++) {
        NimBLEAdvertisedDevice device = foundDevices.getDevice(i);
        String mac = device.getAddress().toString().c_str();
        int rssi = device.getRSSI();
        String name = device.haveName() ? device.getName().c_str() : "(no name)";

        Serial.printf("  %2d: MAC=%s  RSSI=%d dBm  Name=%s\n",
                      i + 1, mac.c_str(), rssi, name.c_str());
    }

    Serial.println("----------------------------------------");
    Serial.printf("Target MAC: %s\n", g_target_mac_lower);

    bool found = false;
    for (int i = 0; i < count; i++) {
        NimBLEAdvertisedDevice device = foundDevices.getDevice(i);
        String mac = device.getAddress().toString().c_str();
        mac = mac.toLowerCase();
        if (mac == g_target_mac_lower) {
            found = true;
            Serial.printf(">>> TARGET FOUND at index %d! RSSI=%d dBm\n",
                          i + 1, device.getRSSI());
            break;
        }
    }

    if (!found) {
        Serial.println(">>> TARGET NOT FOUND in scan results");
    }

    g_pScan->setAdvertisedDeviceCallbacks(&g_scanCallback, false);
    g_pScan->clearResults();

    Serial.println("======================");
}

void debugPrintState() {
    Serial.println("=== BEACON PROXIMITY STATE (NimBLE) ===");
    Serial.printf("Initialized: %s\n", g_initialized ? "YES" : "NO");
    Serial.printf("Scanning enabled: %s\n", g_state.scanning_enabled ? "YES" : "NO");
    Serial.printf("Target MAC: '%s'\n", g_target_mac_lower);
    Serial.println("---");

    Serial.println("[RSSI]");
    Serial.printf("  Raw: %d dBm\n", g_state.rssi_raw);
    Serial.printf("  EMA: %.1f dBm (α=%.2f)\n", g_state.rssi_ema, EMA_ALPHA);
    Serial.printf("  Found: %s\n", g_state.found ? "YES" : "NO");
    Serial.printf("  Last seen: %lu ms ago\n",
                  g_state.last_seen_ms > 0 ? millis() - g_state.last_seen_ms : 0);
    Serial.println("---");

    Serial.println("[ZONE]");
    Serial.printf("  Current: %s\n", zoneToString(g_state.zone));
    Serial.printf("  Pending: %s (count=%d/%d)\n",
                  zoneToString(g_state.pending_zone),
                  g_state.pending_zone_count,
                  ZONE_CHANGE_SAMPLES);
    Serial.printf("  Thresholds: CLOSE>=%d, FAR>=%d, Hysteresis=±%d dB\n",
                  ZONE_CLOSE_THRESHOLD, ZONE_FAR_THRESHOLD, HYSTERESIS_DB);
    Serial.println("---");

    Serial.println("[TREND]");
    Serial.printf("  Current: %s\n", trendToString(g_state.trend));
    Serial.printf("  History samples: %d/%d\n", g_trend_count, TREND_HISTORY_SIZE);
    if (g_trend_count > 0) {
        Serial.print("  Recent EMA: [");
        int show = g_trend_count < 5 ? g_trend_count : 5;
        int start = (g_trend_index - show + TREND_HISTORY_SIZE) % TREND_HISTORY_SIZE;
        for (int i = 0; i < show; i++) {
            int idx = (start + i) % TREND_HISTORY_SIZE;
            Serial.printf("%.1f", g_trend_history[idx]);
            if (i < show - 1) Serial.print(", ");
        }
        Serial.println("]");
    }
    Serial.println("---");

    Serial.println("[TIMING]");
    Serial.printf("  Scan interval: %lums, scan duration: %lus\n",
                  (unsigned long)SCAN_INTERVAL_MS, (unsigned long)SCAN_DURATION_SEC);
    Serial.printf("  Sonar active: %s\n", buzzer::isSonarActive() ? "YES" : "NO");
    Serial.println("---");

    Serial.printf("Distance (legacy): %.2f m\n", g_state.distance_m);
    Serial.printf("NimBLE scan object: %s\n", g_pScan ? "OK" : "NULL!");
    Serial.println("========================================");
}

} // namespace beacon_proximity
