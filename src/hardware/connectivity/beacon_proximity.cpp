#include "hardware/connectivity/beacon_proximity.h"
#include "settings_manager.h"
#include "hardware/buzzer.h"
#include "core/arduino_compat.h"
#include <time.h>          // NimBLEAttValue.h uses time_t without including this itself
#include <NimBLEDevice.h>
#include <esp_log.h>       // esp_log_level_set — silence NimBLE's per-scan INFO chatter
#include <cmath>

namespace beacon_proximity {

// ============================================================================
// Configuration Constants - v2 "Hot/Cold" Detector
// ============================================================================

// ── Scanning (§7.3a — continuous passive scan) ───────────────────────────────
//
// This module used to sample at a hard 2.0 Hz against a tag advertising at 5 Hz,
// so ~60% of every advertisement was thrown away. Three things capped it, and all
// three are gone:
//
//   1. **Controller-side duplicate filtering.** NimBLE reports a given advertiser
//      to onResult *once per scan* while `filter_duplicates` is set. Every
//      subsequent advertisement in the same scan was dropped in
//      `NimBLEScan.cpp` (`if (filter_duplicates && m_callbackSent) return 0;`).
//   2. **`g_pScan->stop()` on the first hit**, which ended the scan the moment the
//      beacon was seen — guaranteeing exactly one sample per scan cycle.
//   3. **A 500 ms scan/idle poll loop** in update().
//
// Now: one scan, started once, running forever (`start(0, ...)` → BLE_HS_FOREVER),
// duplicate filtering off, results not stored, and **passive**.
//
// Passive is not just a power choice — it fixes a real callback deferral. With
// active scanning and a legacy `ADV_IND` advertiser, NimBLE withholds onResult
// until the *scan response* arrives (`NimBLEScan.cpp`, the `m_scan_params.passive
// || !isLegacyAdv || advType != ADV_IND` branch). Passive short-circuits that test
// and delivers on the advertisement itself. This was filed as §7.3d "suspected,
// confirm with runtime logging"; reading the library source confirms it outright,
// so no logging was needed.
//
// Expected: **2 Hz → ~5 Hz**, or ~10 Hz if the tag's advertising interval is
// reconfigured from 200 ms to 100 ms.
static constexpr uint16_t SCAN_ITVL_MS   = 100;  // controller scan interval (ms — this wrapper takes ms, not 0.625ms units)
static constexpr uint16_t SCAN_WINDOW_MS = 100;  // == interval → 100% duty. Costs radio power; zoom-gated to 50m, so bounded.

// ── EMA smoothing (§7.3b — time-based, not sample-based) ─────────────────────
//
// These were α constants tuned against the starved 2 Hz feed. Left alone, raising
// the sample rate would have silently made both EMAs 2.5-5× *faster* and noisier —
// the exact "rate constant nobody re-derived" defect this whole audit was about.
//
// So they are now time constants, with α derived per sample from the **measured**
// elapsed time: α = 1 − e^(−dt/τ). Measured rather than assumed because BLE
// advertising is lossy — samples do not arrive on a schedule, and a dropped one
// must widen that sample's α, not skew the average.
//
// τ = 0.5 s replaces α = 0.4 @ 2 Hz (which was a ~2.5 s time constant), so the
// response is ~5× faster *and* smoother, because it is averaging more samples.
static constexpr float EMA_TAU_S         = 0.5f;   // fast EMA — drives zone, trend, labels
static constexpr float DISPLAY_TAU_S     = 1.0f;   // slow EMA — drives the ring width (analog meter feel)

// Zone thresholds (RSSI-based, not distance-based)
static constexpr int8_t ZONE_CLOSE_THRESHOLD    = -65;  // RSSI >= -65 = CLOSE
static constexpr int8_t ZONE_MEDIUM_THRESHOLD   = -75;  // RSSI >= -75 = MEDIUM
static constexpr int8_t ZONE_FAR_THRESHOLD      = -85;  // RSSI >= -85 = FAR
static constexpr int8_t ZONE_VERY_FAR_THRESHOLD = -90;  // RSSI >= -90 = VERY_FAR
static constexpr int8_t HYSTERESIS_DB = 3;              // ±3 dB band

// Zone confirmation is a *duration*, not a sample count, for the same reason the
// EMAs are. "2 consecutive samples" meant 1.0 s at 2 Hz; at 5 Hz it would have
// quietly become 0.4 s, and at 10 Hz 0.2 s — i.e. no confirmation at all.
static constexpr uint32_t ZONE_CONFIRM_MS = 1000;

// ── Trend detection ──────────────────────────────────────────────────────────
// Diagnostic only (printed by `beacon status` / `beacon trend`; nothing acts on
// it). Slope is now regressed against real time in **dBm/second** rather than
// per-sample, so the thresholds keep their meaning at any rate.
//
// Scale check: walking at 1.4 m/s toward a beacon with path-loss n=2 gives
// dRSSI/dt = 8.686·v/d — about 0.6 dBm/s at 20 m, 1.2 at 10 m, 2.4 at 5 m. So
// 1.0 dBm/s marks "genuinely closing" from ~15 m in. (The old 2.0 dBm/*cycle* at
// 2 Hz was 4.0 dBm/s, which essentially never fired outside 3 m.)
static constexpr float    TREND_APPROACHING_THRESHOLD =  1.0f;  // dBm/s positive
static constexpr float    TREND_DEPARTING_THRESHOLD   = -1.0f;  // dBm/s negative
static constexpr uint32_t TREND_WINDOW_MS   = 4000;  // regression window
static constexpr int      TREND_HISTORY_SIZE = 48;   // ≥ TREND_WINDOW_MS at 10Hz + margin

// Lost beacon timeout. 15 s made sense when a sample was a 500 ms scan cycle that
// could legitimately miss several times; at 5-10 Hz, 5 s is already 25-50 missed
// advertisements, which is unambiguous.
static constexpr uint32_t BEACON_LOST_TIMEOUT_MS = 5000;

// ============================================================================
// Module State
// ============================================================================

static BeaconState g_state;
static bool g_initialized = false;
static bool g_sonar_suppressed = false;  // True when waypoint fix has sonar priority
static bool g_found = false;             // True when user has tapped the ball (in-session only)
static NimBLEScan* g_pScan = nullptr;
static bool g_scan_running = false;      // Continuous scan started (§7.3a)

// Target beacon (lowercase for comparison / display)
static char g_target_mac_lower[18] = "";
// Same address parsed once. With duplicate filtering off, onResult now fires for
// every advertisement from every device in range, so the old
// `getAddress().toString()` + String compare — which heap-allocated twice per
// callback — is no longer an acceptable way to reject non-target traffic.
static NimBLEAddress g_target_addr{};
static bool g_target_addr_valid = false;

// Spinlock protecting g_state fields written by NimBLE callback and read by Network Task
static portMUX_TYPE g_state_mux = portMUX_INITIALIZER_UNLOCKED;

// Timestamp of the previous accepted sample, for the τ-based EMA. 0 = none yet.
static uint32_t g_last_sample_ms = 0;

// Diagnostics for "the beacon reads -127 and nothing else is observable".
// g_adv_callbacks counts *every* advertisement delivered to onResult, before the
// address filter; g_target_callbacks counts only ones that matched. The pair
// distinguishes a scan that is delivering nothing at all from one that is
// delivering plenty but never the target — different faults, same symptom.
static volatile uint32_t g_adv_callbacks    = 0;
static volatile uint32_t g_target_callbacks = 0;
static bool     g_raw_logging      = false;
static uint32_t g_last_raw_log_ms  = 0;

// Trend history circular buffer — timestamped, so the regression is against real
// time rather than sample index.
struct TrendSample { uint32_t t_ms; float ema; };
static TrendSample g_trend_history[TREND_HISTORY_SIZE];
static int g_trend_index = 0;
static int g_trend_count = 0;  // How many samples in history


// ============================================================================
// Forward Declarations
// ============================================================================

/**
 * @brief Load the configured beacon MAC into the comparison forms used at runtime.
 *
 * Called from both init() and setEnabled(). It used to run only in setEnabled(),
 * which meant that with scanning inactive — i.e. any zoom other than 50 m —
 * g_target_mac_lower was empty and debugScanAll() compared every device against
 * "", reporting a confident ">>> TARGET NOT FOUND" that was structurally
 * incapable of ever finding anything. That is exactly the state you are in when
 * reaching for `beacon scan` to debug a missing beacon.
 */
static void applyTargetFromSettings();
static void updateRSSI_EMA(int8_t raw, uint32_t now);
static void calculateTrend();
static void updateZone(float ema, uint32_t now, bool& zone_changed_out, ProximityZone& changed_to_out);
static ProximityZone classifyRSSI(float rssi, ProximityZone current_zone);

// ============================================================================
// BLE Scan Callback
// ============================================================================

class BeaconScanCallback : public NimBLEAdvertisedDeviceCallbacks {
    void onResult(NimBLEAdvertisedDevice* advertisedDevice) override {
        // Diagnostic counter, before any filtering: separates "the scan is not
        // delivering anything" from "it is delivering, but never our target".
        // Those two have completely different causes and the distinction is not
        // otherwise observable from outside.
        g_adv_callbacks++;

        if (g_raw_logging) {
            // Throttled — with duplicates off in a busy environment this fires
            // for every advertisement from every device, which would flood the
            // console from the NimBLE host task.
            const uint32_t now_ms = millis();
            if ((uint32_t)(now_ms - g_last_raw_log_ms) >= 50) {
                g_last_raw_log_ms = now_ms;
                Serial.printf("[BLE-RAW] %s rssi=%d advType=%u%s\n",
                              advertisedDevice->getAddress().toString().c_str(),
                              advertisedDevice->getRSSI(),
                              (unsigned)advertisedDevice->getAdvType(),
                              (g_target_addr_valid &&
                               advertisedDevice->getAddress() == g_target_addr) ? "  <== TARGET" : "");
            }
        }

        // Hot path. With duplicate filtering disabled this runs for every
        // advertisement from every device in range, which in a busy RF
        // environment is far more often than the ~5 Hz we care about — so reject
        // by address comparison, before touching the heap.
        if (!g_target_addr_valid) return;
        if (!(advertisedDevice->getAddress() == g_target_addr)) return;

        g_target_callbacks++;

        const int rssi = advertisedDevice->getRSSI();
        const uint32_t now = millis();

        portENTER_CRITICAL(&g_state_mux);
        g_state.rssi_raw = (int8_t)rssi;
        g_state.found = true;
        g_state.last_seen_ms = now;
        updateRSSI_EMA((int8_t)rssi, now);
        portEXIT_CRITICAL(&g_state_mux);

        // No stop() here. Stopping on the first hit was one of the three things
        // capping the feed at one sample per scan cycle — see §7.3a above.
    }
};

static BeaconScanCallback g_scanCallback;

// ============================================================================
// EMA Smoothing
// ============================================================================

// Called with g_state_mux held.
static void updateRSSI_EMA(int8_t raw, uint32_t now) {
    if (g_state.rssi_ema <= -127.0f || g_last_sample_ms == 0) {
        // First reading — adopt it rather than ramping in from -127
        g_state.rssi_ema = (float)raw;
        g_state.rssi_display = (float)raw;
    } else {
        const float gap_ms = (float)(now - g_last_sample_ms);

        // α from the *measured* gap, so a dropped advertisement widens that
        // sample's weight instead of skewing the effective time constant.
        const float dt_s = gap_ms / 1000.0f;
        const float a_fast = 1.0f - expf(-dt_s / EMA_TAU_S);
        const float a_slow = 1.0f - expf(-dt_s / DISPLAY_TAU_S);

        g_state.rssi_ema += a_fast * ((float)raw - g_state.rssi_ema);
        // Display EMA: second-stage slow smoothing on rssi_ema for the gauge ring
        g_state.rssi_display += a_slow * (g_state.rssi_ema - g_state.rssi_display);

        // Measured mean inter-arrival — the direct read-out of whether §7.3a
        // worked. Reported by `beacon status`. Smoothed heavily (α=0.1) because
        // BLE inter-arrival is spiky by nature.
        if (g_state.sample_interval_ms <= 0.0f) g_state.sample_interval_ms = gap_ms;
        else g_state.sample_interval_ms += 0.1f * (gap_ms - g_state.sample_interval_ms);
    }
    g_last_sample_ms = now;

    // Add to trend history (timestamped)
    g_trend_history[g_trend_index].t_ms = now;
    g_trend_history[g_trend_index].ema  = g_state.rssi_ema;
    g_trend_index = (g_trend_index + 1) % TREND_HISTORY_SIZE;
    if (g_trend_count < TREND_HISTORY_SIZE) {
        g_trend_count++;
    }
}

// ============================================================================
// Trend Detection (Linear Regression)
// ============================================================================

// Regresses RSSI against **seconds**, over the samples inside TREND_WINDOW_MS.
// The old version regressed against sample *index*, which silently redefined the
// dBm/cycle thresholds every time the sample rate moved.
//
// Takes a snapshot of the history under the spinlock and computes outside it —
// a 48-point two-pass regression is far too long to hold a critical section for.
static void calculateTrend() {
    TrendSample snap[TREND_HISTORY_SIZE];
    int n = 0;

    portENTER_CRITICAL(&g_state_mux);
    const int count = g_trend_count;
    const int index = g_trend_index;
    const uint32_t now = millis();
    for (int i = 0; i < count; i++) {
        const int idx = (index - count + i + TREND_HISTORY_SIZE) % TREND_HISTORY_SIZE;
        // Keep only what falls inside the regression window
        if ((uint32_t)(now - g_trend_history[idx].t_ms) <= TREND_WINDOW_MS) {
            snap[n++] = g_trend_history[idx];
        }
    }
    portEXIT_CRITICAL(&g_state_mux);

    if (n < 3) {
        g_state.trend = MovementTrend::UNKNOWN;
        return;
    }

    // Least squares: slope = Σ((x-x̄)(y-ȳ)) / Σ((x-x̄)²), x in seconds relative to
    // the oldest retained sample (keeps the magnitudes small and well-conditioned).
    const uint32_t t0 = snap[0].t_ms;
    float sum_x = 0, sum_y = 0;
    for (int i = 0; i < n; i++) {
        sum_x += (float)(snap[i].t_ms - t0) / 1000.0f;
        sum_y += snap[i].ema;
    }
    const float mean_x = sum_x / n;
    const float mean_y = sum_y / n;

    float numerator = 0, denominator = 0;
    for (int i = 0; i < n; i++) {
        const float x_diff = (float)(snap[i].t_ms - t0) / 1000.0f - mean_x;
        const float y_diff = snap[i].ema - mean_y;
        numerator += x_diff * y_diff;
        denominator += x_diff * x_diff;
    }

    if (denominator < 0.001f) {
        g_state.trend = MovementTrend::STABLE;
        return;
    }

    const float slope = numerator / denominator;   // dBm per second

    // Positive slope = RSSI increasing = getting closer
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

// Confirmation is a duration, not a sample count — see ZONE_CONFIRM_MS.
static void updateZone(float ema, uint32_t now, bool& zone_changed_out, ProximityZone& changed_to_out) {
    // Classify what zone the current EMA would put us in
    ProximityZone new_zone = classifyRSSI(ema, g_state.zone);
    zone_changed_out = false;

    if (new_zone == g_state.zone) {
        // Same zone — drop any pending change
        g_state.pending_zone = g_state.zone;
        g_state.pending_since_ms = 0;
    } else if (new_zone == g_state.pending_zone) {
        // Same candidate as last time — commit once it has held long enough
        if (g_state.pending_since_ms != 0 &&
            (uint32_t)(now - g_state.pending_since_ms) >= ZONE_CONFIRM_MS) {
            g_state.zone = new_zone;
            g_state.pending_since_ms = 0;
            zone_changed_out = true;
            changed_to_out = new_zone;
            // Note: Serial.printf intentionally NOT called here — caller logs outside spinlock
        }
    } else {
        // New candidate — start its hold timer
        g_state.pending_zone = new_zone;
        g_state.pending_since_ms = now;
    }
}

// ============================================================================
// Public API Implementation
// ============================================================================

static void applyTargetFromSettings() {
    const auto& settings = settings_manager::getSettings();
    g_target_mac_lower[0] = '\0';
    g_target_addr_valid = false;

    if (settings.beacon_count == 0 || strlen(settings.beacon_macs[0]) == 0) return;

    strncpy(g_target_mac_lower, settings.beacon_macs[0], sizeof(g_target_mac_lower) - 1);
    g_target_mac_lower[sizeof(g_target_mac_lower) - 1] = '\0';
    for (int i = 0; g_target_mac_lower[i]; i++) {
        g_target_mac_lower[i] = tolower((unsigned char)g_target_mac_lower[i]);
    }

    // Parse once, so the hot callback never has to build a String.
    // NimBLEAddress::operator== is a memcmp over the 6 bytes only — it ignores
    // the address type, so a random-static tag compares fine against an address
    // parsed with the default BLE_ADDR_PUBLIC.
    g_target_addr = NimBLEAddress(std::string(g_target_mac_lower));
    g_target_addr_valid = true;
}

void init() {
    if (g_initialized) return;

    Serial.println("[BEACON] Initializing beacon proximity (NimBLE)...");

    // NimBLE stack needs ~25KB of internal SRAM. Check before attempting —
    // partial init failure at very low heap corrupts memory and hangs the UI task.
    // Log free SRAM for diagnostics — init is called at boot before WiFi/tasks consume SRAM
    size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    Serial.printf("[BEACON] Free internal SRAM before BLE init: %u bytes\n", (unsigned)free_internal);

    if (!NimBLEDevice::getInitialized()) {
        // Silence the stack's own INFO chatter before bringing it up. Beacon proximity
        // restarts discovery roughly once a second while at 50m zoom, and NimBLE logs
        // four lines per "GAP procedure initiated" — ~4 lines/sec of serial that carries
        // no information we act on, on a console path where every write ends in fflush.
        // Warnings and errors still come through.
        esp_log_level_set("NimBLE", ESP_LOG_WARN);
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

    // ⚠️ Order matters: setAdvertisedDeviceCallbacks(cb, wantDuplicates) internally
    // calls setDuplicateFilter(!wantDuplicates), so it would silently re-enable
    // filtering if it ran after the explicit call below. Both are written out for
    // clarity rather than relying on that side effect.
    g_pScan->setAdvertisedDeviceCallbacks(&g_scanCallback, true /*wantDuplicates*/);
    g_pScan->setDuplicateFilter(false);   // every advertisement, not one per scan
    g_pScan->setActiveScan(false);        // passive — also avoids scan-response callback deferral
    g_pScan->setMaxResults(0);            // don't accumulate a results vector; callback only
    g_pScan->setInterval(SCAN_ITVL_MS);
    g_pScan->setWindow(SCAN_WINDOW_MS);

    g_initialized = true;

    // Load the target now, not only at setEnabled(), so `beacon scan` can
    // meaningfully report whether the target is visible at any zoom level.
    applyTargetFromSettings();

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

        applyTargetFromSettings();

        // Reset all state
        resetState();

        g_state.scanning_enabled = true;

        // Start the single continuous scan (§7.3a). duration 0 → BLE_HS_FOREVER.
        if (g_pScan && !g_scan_running) {
            if (g_pScan->start(0, nullptr, false)) {
                g_scan_running = true;
            } else {
                Serial.println("[BEACON] WARNING: continuous scan failed to start");
            }
        }

        Serial.printf("[BEACON] Proximity scanning ENABLED for %s\n", settings.beacon_macs[0]);
        Serial.printf("[BEACON] Continuous passive scan: %ums window / %ums interval, no duplicate filter\n",
                      (unsigned)SCAN_WINDOW_MS, (unsigned)SCAN_ITVL_MS);

    } else if (!enabled && g_state.scanning_enabled) {
        g_state.scanning_enabled = false;
        g_state.found = false;
        if (g_pScan && g_scan_running) {
            g_pScan->stop();
            g_scan_running = false;
        }
        buzzer::stopSonar();  // Stop immediately — don't wait for Network Task's updateSonar()
        Serial.println("[BEACON] Proximity scanning DISABLED");
    }
}

bool isEnabled() {
    return g_state.scanning_enabled;
}

void setRawLogging(bool enabled) {
    g_raw_logging = enabled;
    Serial.printf("[BEACON] Raw advertisement logging %s\n", enabled ? "ON" : "OFF");
}

void getCallbackCounts(uint32_t& all_out, uint32_t& target_out) {
    all_out    = g_adv_callbacks;
    target_out = g_target_callbacks;
}

bool isInRange() {
    return g_state.scanning_enabled
        && !g_found
        && g_state.zone != ProximityZone::OUT_OF_RANGE;
}

void resetState() {
    // Reset RSSI tracking
    g_state.rssi_raw = -127;
    g_state.rssi_ema = -127.0f;
    g_state.rssi_display = -127.0f;
    g_state.found = false;

    g_state.sample_interval_ms = 0.0f;

    // Reset zone state
    g_state.zone = ProximityZone::OUT_OF_RANGE;
    g_state.pending_zone = ProximityZone::OUT_OF_RANGE;
    g_state.pending_since_ms = 0;

    // Reset trend state
    g_state.trend = MovementTrend::UNKNOWN;
    g_trend_index = 0;
    g_trend_count = 0;
    for (int i = 0; i < TREND_HISTORY_SIZE; i++) {
        g_trend_history[i].t_ms = 0;
        g_trend_history[i].ema  = -127.0f;
    }

    // Reset timing
    g_state.last_seen_ms = 0;
    g_state.cycle_start_ms = 0;
    g_last_sample_ms = 0;

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

    // Check if beacon was lost
    bool beacon_lost = false;
    portENTER_CRITICAL(&g_state_mux);
    if (g_state.last_seen_ms > 0 && (now - g_state.last_seen_ms) > BEACON_LOST_TIMEOUT_MS) {
        if (g_state.zone != ProximityZone::OUT_OF_RANGE || g_state.rssi_ema > -127.0f) {
            beacon_lost = true;
            g_state.zone = ProximityZone::OUT_OF_RANGE;
            g_state.pending_zone = ProximityZone::OUT_OF_RANGE;
            g_state.pending_since_ms = 0;
            g_state.trend = MovementTrend::UNKNOWN;
            g_trend_count = 0;
            g_state.rssi_ema = -127.0f;
            g_state.rssi_display = -127.0f;
            g_state.rssi_raw = -127;
            g_state.sample_interval_ms = 0.0f;
            g_last_sample_ms = 0;
        }
    }
    portEXIT_CRITICAL(&g_state_mux);
    if (beacon_lost) {
        Serial.println("[BEACON] Beacon lost - resetting to OUT_OF_RANGE");
    }

    // The scan runs continuously now, so there is no scan-completion event to hang
    // derived state off. Zone/trend/distance are instead recomputed on this task's
    // own cadence (Network Task loop), reading whatever the EMA has accumulated.
    // That decouples them from sample arrival, which is exactly what the
    // time-based confirmation and τ-based EMA were changed to allow.
    //
    // The results-sweep block that used to live here is gone with the polling:
    // it existed to catch a beacon the callback had missed within a finite scan,
    // and setMaxResults(0) means there is no results vector to sweep anyway.
    calculateTrend();   // snapshots under the lock internally

    bool zone_changed = false;
    ProximityZone zone_changed_to = ProximityZone::OUT_OF_RANGE;
    const auto& settings = settings_manager::getSettings();
    portENTER_CRITICAL(&g_state_mux);
    if (g_state.rssi_ema > -127.0f) {
        updateZone(g_state.rssi_ema, now, zone_changed, zone_changed_to);
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

    // Restart the scan if it stopped for any reason (host reset, error).
    if (g_pScan && g_scan_running && !g_pScan->isScanning()) {
        if (g_pScan->start(0, nullptr, false)) {
            Serial.println("[BEACON] Continuous scan restarted");
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

    // ── Tempo: continuous in RSSI, not four zone steps ───────────────────────
    //
    // This used to be a switch on the confirmed zone — 1500/750/500/250 ms — which
    // is the same defect the waypoint sonar had, and it drew the same field report:
    // *"the rate at which the beeping changes is very difficult to gauge where to
    // go"*. With four steps, most of a search happens inside one zone, where moving
    // produces **no audible change at all**. That is fatal here specifically,
    // because a human hunting a beacon is not reading absolute loudness — they are
    // listening for *change* in response to their own movement, and a step function
    // gives them none until they happen to cross a boundary.
    //
    // Linear in dBm is the right curve. RSSI ≈ C − 20·log₁₀(d), so equal steps in
    // dBm are equal *ratios* of distance — which is exactly the geometric mapping
    // the waypoint sonar uses over metres, arrived at from the other direction.
    //
    // The zone is still what decides *whether* to beep (that decision is genuinely
    // discrete, and keeps its hysteresis); it no longer decides how fast.
    constexpr float SONAR_RSSI_FAR  = -90.0f;   // slowest tempo at/below
    constexpr float SONAR_RSSI_NEAR = -50.0f;   // fastest tempo at/above
    constexpr float SONAR_MS_FAR    = 1500.0f;
    constexpr float SONAR_MS_NEAR   = 150.0f;

    float t = (g_state.rssi_ema - SONAR_RSSI_FAR) / (SONAR_RSSI_NEAR - SONAR_RSSI_FAR);
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    const uint32_t interval_ms =
        (uint32_t)(SONAR_MS_FAR * powf(SONAR_MS_NEAR / SONAR_MS_FAR, t) + 0.5f);

    // ── Timbre: beep length encodes the trend ────────────────────────────────
    //
    // "Warmer / colder" is far more actionable than absolute level when hunting,
    // because absolute RSSI depends on the environment, the tag's orientation and
    // your own body, none of which the user knows — but the *sign of the change*
    // in response to a step is meaningful regardless. The trend was already being
    // computed and read by nothing at all.
    //
    // The buzzer is a bare on/off line through the IO expander, so there is no
    // pitch to modulate — but beep *duration* is already a parameter of
    // setSonarInterval(), so this costs nothing: a 12 ms tick and a 60 ms tone are
    // unmistakably different on a piezo.
    uint16_t beep_ms = 30;   // STABLE / UNKNOWN — neutral
    switch (g_state.trend) {
        case MovementTrend::APPROACHING: beep_ms = 60; break;  // warmer — a fuller tone
        case MovementTrend::DEPARTING:   beep_ms = 12; break;  // colder — a clipped tick
        default: break;
    }

    buzzer::setSonarInterval(interval_ms, beep_ms);
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

    // This is a one-shot *active* scan that stores results — the opposite of the
    // continuous passive scan the module normally runs. Stop that first, and be
    // careful to restore every parameter afterwards (see the restore block below).
    const bool was_running = g_scan_running;
    if (was_running) {
        g_pScan->stop();
        g_scan_running = false;
    }

    g_pScan->setAdvertisedDeviceCallbacks(nullptr, false);
    g_pScan->setActiveScan(true);
    g_pScan->setMaxResults(0xFF);   // this scan *wants* the results vector
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

        // HCI address type only says public vs random; for random addresses the
        // *sub*-type lives in the top two bits of the MSB, and that is the bit
        // that matters here — a resolvable-private address rotates every ~15 min,
        // so it can never be matched by a fixed MAC, whereas a random-static one
        // is stable for the life of the device.
        //   11 = static, 01 = resolvable private, 00 = non-resolvable private
        // advType: 0=ADV_IND (connectable, typical of phones), 3=ADV_NONCONN_IND
        // (broadcast-only — what a beacon/tag does), 4=SCAN_RSP.
        const uint8_t atype = device.getAddressType();
        const uint8_t msb   = device.getAddress().getNative()[5];  // native order is little-endian
        const char* atype_s;
        if (atype == 0) {
            atype_s = "public";
        } else {
            switch (msb >> 6) {
                case 0b11: atype_s = "random-static";  break;
                case 0b01: atype_s = "resolvable-priv"; break;
                case 0b00: atype_s = "non-resolvable"; break;
                default:   atype_s = "random-?";       break;
            }
        }
        Serial.printf("  %2d: MAC=%s  RSSI=%4d dBm  addr=%-18s adv=%u  Name=%s\n",
                      i + 1, mac.c_str(), rssi, atype_s,
                      (unsigned)device.getAdvType(), name.c_str());
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

    // Restore the continuous-scan configuration in full. ⚠️ Every line here
    // matters: setAdvertisedDeviceCallbacks(cb, false) calls
    // setDuplicateFilter(true) internally, so passing `true` — or re-clearing the
    // filter afterwards — is required, otherwise this debug command would leave
    // the beacon feed permanently back at one sample per scan.
    g_pScan->setAdvertisedDeviceCallbacks(&g_scanCallback, true /*wantDuplicates*/);
    g_pScan->setDuplicateFilter(false);
    g_pScan->setActiveScan(false);
    g_pScan->setMaxResults(0);
    g_pScan->clearResults();

    if (was_running) {
        if (g_pScan->start(0, nullptr, false)) {
            g_scan_running = true;
        } else {
            Serial.println("[BLE] WARNING: failed to resume continuous scan");
        }
    }

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
    Serial.printf("  EMA: %.1f dBm (τ=%.2fs)\n", g_state.rssi_ema, EMA_TAU_S);
    Serial.printf("  Display EMA: %.1f dBm (τ=%.2fs — drives ring width)\n",
                  g_state.rssi_display, DISPLAY_TAU_S);
    Serial.printf("  Found: %s\n", g_state.found ? "YES" : "NO");
    Serial.printf("  Last seen: %lu ms ago\n",
                  g_state.last_seen_ms > 0 ? millis() - g_state.last_seen_ms : 0);
    Serial.printf("  Sample rate: %.2f Hz (mean gap %.0f ms)\n",
                  g_state.sample_interval_ms > 0.0f ? 1000.0f / g_state.sample_interval_ms : 0.0f,
                  g_state.sample_interval_ms);
    Serial.printf("  Scan callbacks since boot: %lu total, %lu matched target\n",
                  (unsigned long)g_adv_callbacks, (unsigned long)g_target_callbacks);
    if (g_adv_callbacks == 0) {
        Serial.println("  >>> ZERO advertisements delivered — the scan itself is not working");
    } else if (g_target_callbacks == 0) {
        Serial.println("  >>> Scan works, but target MAC never seen — wrong MAC, or tag not advertising");
    }
    Serial.println("---");

    Serial.println("[ZONE]");
    Serial.printf("  Current: %s\n", zoneToString(g_state.zone));
    Serial.printf("  Pending: %s (held %lu/%lu ms)\n",
                  zoneToString(g_state.pending_zone),
                  (unsigned long)(g_state.pending_since_ms ? millis() - g_state.pending_since_ms : 0),
                  (unsigned long)ZONE_CONFIRM_MS);
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
            Serial.printf("%.1f", g_trend_history[idx].ema);
            if (i < show - 1) Serial.print(", ");
        }
        Serial.println("]");
    }
    Serial.printf("  Thresholds: ±%.1f dBm/s over a %lums window\n",
                  TREND_APPROACHING_THRESHOLD, (unsigned long)TREND_WINDOW_MS);
    Serial.println("---");

    Serial.println("[TIMING]");
    Serial.printf("  Continuous passive scan: %ums window / %ums interval, running: %s\n",
                  (unsigned)SCAN_WINDOW_MS, (unsigned)SCAN_ITVL_MS,
                  (g_pScan && g_pScan->isScanning()) ? "YES" : "no");
    Serial.printf("  Sonar active: %s\n", buzzer::isSonarActive() ? "YES" : "NO");
    Serial.println("---");

    Serial.printf("Distance (legacy): %.2f m\n", g_state.distance_m);
    Serial.printf("NimBLE scan object: %s\n", g_pScan ? "OK" : "NULL!");
    Serial.println("========================================");
}

} // namespace beacon_proximity
