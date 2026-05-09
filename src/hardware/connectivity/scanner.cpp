#include "scanner.h"
#include "system_config.h"
#include "core/arduino_compat.h"
#include "esp_wifi.h"
#include "esp_event.h"

// Note: BLE scanning handled by beacon_proximity.cpp independently

namespace scanner {

static int wifi_count = SCAN_FAILED;
static bool wifi_enabled = true;

static uint32_t last_wifi_scan = 0;
static bool scan_in_progress = false;

// SCAN_DONE event flag — set by WIFI_EVENT_SCAN_DONE handler, cleared on result read.
// Using the event (rather than polling esp_wifi_scan_get_ap_num) is critical: the
// polling approach returns ESP_OK with a partial channel count at 500ms, causing the
// code to read results before channels 5-13 are scanned and missing strong nearby APs
// on channels 6 and 11.  The event fires only after ALL channels are done.
static volatile bool s_scan_done = false;
static uint32_t s_scan_start_ms = 0;

// AP records captured after each scan completes
static APRecord s_ap_records[MAX_SCAN_APS];
static uint16_t s_ap_record_count = 0;

// ── WIFI_EVENT_SCAN_DONE handler ──────────────────────────────────────────────
// Runs in the WiFi event-loop task (not the UI task).  bool write is atomic on
// Xtensa, so no mutex needed for this single-producer/single-consumer flag.
static void onScanDone(void*, esp_event_base_t, int32_t, void*) {
    if (scan_in_progress) {  // only accept events for scans WE started
        s_scan_done = true;
    }
}

static void collectResults() {
    uint16_t ap_count = 0;
    if (esp_wifi_scan_get_ap_num(&ap_count) != ESP_OK) {
        wifi_count = SCAN_FAILED;
        scan_in_progress = false;
        return;
    }
    uint16_t fetch_count = (ap_count < MAX_SCAN_APS) ? ap_count : MAX_SCAN_APS;
    if (fetch_count > 0) {
        wifi_ap_record_t raw[MAX_SCAN_APS];
        if (esp_wifi_scan_get_ap_records(&fetch_count, raw) == ESP_OK) {
            s_ap_record_count = fetch_count;
            for (uint16_t j = 0; j < fetch_count; j++) {
                strncpy(s_ap_records[j].ssid, (char*)raw[j].ssid, 32);
                s_ap_records[j].ssid[32] = '\0';
                s_ap_records[j].rssi     = raw[j].rssi;
                s_ap_records[j].authmode = raw[j].authmode;
            }
        }
    } else {
        s_ap_record_count = 0;
    }
    wifi_count = (int)fetch_count;
    scan_in_progress = false;
    Serial.printf("[SCAN] Collected %d APs after SCAN_DONE\n", wifi_count);
}

void init() {
    static bool initialized = false;
    if (initialized) return;
    initialized = true;

    // Register SCAN_DONE handler before starting any scan so we never miss the event.
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_SCAN_DONE, onScanDone, nullptr);

    // WiFi is already initialized by wifi_manager::init()
    // Attempt initial async scan; may fail if esp_wifi_start() not called yet — that's OK.
    wifi_scan_config_t scan_cfg = {};
    scan_cfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    if (esp_wifi_scan_start(&scan_cfg, false) == ESP_OK) {
        scan_in_progress = true;
        s_scan_done      = false;
        s_scan_start_ms  = millis();
    }
    last_wifi_scan = millis();
    Serial.println("[SCAN] WiFi scan initialized");
    Serial.println("[BLE] Stubbed — re-enable with esp-nimble-cpp component");
}

void update() {
    if (!wifi_enabled) return;

    uint32_t now = millis();

    if (scan_in_progress) {
        if (s_scan_done) {
            // SCAN_DONE event confirms ALL channels are done — safe to read full results.
            s_scan_done = false;
            collectResults();
        } else if ((now - s_scan_start_ms) > 10000) {
            // Safety net: give up after 10 s (scan should never take this long).
            Serial.println("[SCAN] Scan timeout — giving up");
            s_scan_done       = false;
            scan_in_progress  = false;
            wifi_count        = SCAN_FAILED;
        }
    }

    if (!scan_in_progress &&
        (now - last_wifi_scan >= system_config::timing::WIFI_SCAN_INTERVAL_MS)) {
        wifi_scan_config_t scan_cfg = {};
        scan_cfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;
        if (esp_wifi_scan_start(&scan_cfg, false) == ESP_OK) {
            scan_in_progress = true;
            s_scan_done      = false;
            s_scan_start_ms  = now;
        }
        last_wifi_scan = now;
    }
}

int getWiFiCount() { return wifi_count; }

void setWiFiEnabled(bool enabled) { wifi_enabled = enabled; }

bool isWiFiEnabled() { return wifi_enabled; }

void triggerWiFiScan() {
    if (scan_in_progress) {
        Serial.println("[SCAN] Scan already in progress - waiting for results");
        return;
    }
    // Clear any stale SCAN_DONE event (e.g. from the auto-connect internal scan)
    // before starting ours, so the handler only fires for our scan.
    s_scan_done = false;
    wifi_scan_config_t scan_cfg = {};
    scan_cfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    if (esp_wifi_scan_start(&scan_cfg, false) == ESP_OK) {
        scan_in_progress = true;
        s_scan_start_ms  = millis();
        last_wifi_scan   = millis();
        Serial.println("[SCAN] Manual WiFi scan triggered");
    } else {
        Serial.println("[SCAN] Failed to start scan (WiFi not ready?)");
    }
}

int scanComplete() {
    return scan_in_progress ? SCAN_RUNNING : wifi_count;
}

const APRecord* getScanRecord(int index) {
    if (index < 0 || index >= (int)s_ap_record_count) return nullptr;
    return &s_ap_records[index];
}

} // namespace scanner
