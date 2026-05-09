#include "wifi_manager.h"
#include "settings_manager.h"
#include "core/arduino_compat.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "lwip/dns.h"

namespace wifi_manager {

static ConnectionStatus current_status = ConnectionStatus::DISCONNECTED;
static bool auto_reconnect_enabled = true;
static bool wifi_enabled = false;
static bool wifi_ever_enabled = false;  // Session flag — never cleared; NimBLE guard
static bool is_initialized = false;

static uint32_t connection_start_time = 0;
static uint32_t connection_timeout_ms = 15000;
static uint32_t last_update_time = 0;

static String saved_ssid;
static String saved_password;

static esp_netif_t* sta_netif = nullptr;

static void updateStatus(ConnectionStatus s) {
    if (s != current_status) {
        current_status = s;
        Serial.printf("[WIFI_MGR] Status: %s\n", getStatusString().c_str());
    }
}

static bool hasTimedOut() {
    if (connection_start_time == 0) return false;
    return (millis() - connection_start_time) > connection_timeout_ms;
}

// ============================================================================
// ESP-IDF event handler (runs in system event task)
// ============================================================================
static void wifi_event_handler(void* arg, esp_event_base_t base,
                               int32_t event_id, void* event_data) {
    if (base == WIFI_EVENT) {
        if (event_id == WIFI_EVENT_STA_START) {
            if (sta_netif) esp_netif_set_hostname(sta_netif, "radar-display");
            // Only auto-connect when STA mode is explicitly enabled.
            // AP mode (APSTA) also fires WIFI_EVENT_STA_START, but wifi_enabled
            // stays false — skipping esp_wifi_connect() here prevents the STA
            // from channel-hopping to find a saved home network, which starves
            // the AP of radio time and breaks the WPA2 4-way handshake.
            if (wifi_enabled && !saved_ssid.isEmpty()) {
                esp_wifi_connect();
            }
        } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            // Ignore STA disconnect events when not in STA mode (AP mode uses APSTA
            // so STA disconnect events fire but should not trigger reconnect logic).
            if (!wifi_enabled) return;

            if (current_status == ConnectionStatus::CONNECTED) {
                if (auto_reconnect_enabled && !saved_ssid.isEmpty()) {
                    updateStatus(ConnectionStatus::RECONNECTING);
                    connection_start_time = millis();
                    esp_wifi_connect();
                } else {
                    updateStatus(ConnectionStatus::DISCONNECTED);
                }
            } else if (current_status == ConnectionStatus::RECONNECTING) {
                // Keep trying until timeout
                esp_wifi_connect();
            }
        }
    } else if (base == IP_EVENT) {
        if (event_id == IP_EVENT_STA_GOT_IP) {
            connection_start_time = 0;
            updateStatus(ConnectionStatus::CONNECTED);
            Serial.printf("[WIFI_MGR] Connected! IP: %s\n", getIPAddress().c_str());
        }
    }
}

// ============================================================================
// Public API
// ============================================================================

bool init() {
    if (is_initialized) return true;

    esp_netif_init();
    esp_event_loop_create_default();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    // Use default buffer sizes (static_rx_buf_num=10, mgmt_sbuf_num=32).
    // WiFi boot modes (AP and STA) are now separate boot paths — NimBLE never starts
    // when WiFi is active. The old DMA SRAM fragmentation concern (NimBLE pre-alloc
    // fragmenting the heap before WiFi init) no longer applies.
    // Default RX buffers are critical for scan quality: 14 APs respond simultaneously
    // to probe requests; with only 3 buffers most frames were dropped → 4-8 results
    // instead of the expected 14+.
    cfg.dynamic_tx_buf_num = 32; // keep at default — needed for large HTML responses (AP mode)
    esp_err_t ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) {
        Serial.printf("[WIFI_MGR] esp_wifi_init failed: %d — heap fragmented? Init too late?\n", (int)ret);
        return false;
    }

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        wifi_event_handler, nullptr, nullptr);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        wifi_event_handler, nullptr, nullptr);

    // NOTE: No mode, no netif, no esp_wifi_start() here.
    // Mode and netif are set by whoever activates WiFi (setEnabled for STA,
    // gpx_server::start for AP). This mirrors the Arduino softAP() approach —
    // setting WIFI_MODE_STA here before AP mode corrupts the AP WPA2 state machine.

    is_initialized = true;
    last_update_time = millis();
    Serial.println("[WIFI_MGR] Stack allocated (dormant — call setEnabled(true) to start radio)");
    return true;
}

bool connect(const String& ssid, const String& password, uint32_t timeout_ms) {
    if (!is_initialized || !wifi_enabled || ssid.isEmpty()) return false;

    settings_manager::saveWiFi(ssid, password);
    saved_ssid = ssid;
    saved_password = password;

    wifi_config_t wifi_config = {};
    strncpy((char*)wifi_config.sta.ssid, ssid.c_str(), sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char*)wifi_config.sta.password, password.c_str(), sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = password.isEmpty() ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;

    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);

    connection_timeout_ms = timeout_ms;
    connection_start_time = millis();
    updateStatus(ConnectionStatus::CONNECTING);

    esp_wifi_disconnect();
    esp_wifi_connect();
    Serial.printf("[WIFI_MGR] Connecting to: %s\n", ssid.c_str());
    return true;
}

bool autoConnect(uint32_t timeout_ms) {
    if (!is_initialized || !wifi_enabled) return false;

    String ssid, password;
    if (!settings_manager::getWiFi(ssid, password)) {
        Serial.println("[WIFI_MGR] No saved credentials");
        return false;
    }
    return connect(ssid, password, timeout_ms);
}

bool disconnect(bool forget) {
    if (!is_initialized) return false;

    esp_wifi_disconnect();
    if (forget) {
        settings_manager::clearWiFi();
        saved_ssid = "";
        saved_password = "";
    }
    connection_start_time = 0;
    updateStatus(ConnectionStatus::DISCONNECTED);
    return true;
}

bool isConnected() {
    wifi_ap_record_t ap;
    return esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
}

ConnectionStatus getConnectionStatus() { return current_status; }

String getStatusString() {
    switch (current_status) {
        case ConnectionStatus::DISCONNECTED:  return "Disconnected";
        case ConnectionStatus::CONNECTING:    return "Connecting...";
        case ConnectionStatus::CONNECTED:     return "Connected";
        case ConnectionStatus::FAILED:        return "Connection Failed";
        case ConnectionStatus::RECONNECTING:  return "Reconnecting...";
        default:                              return "Unknown";
    }
}

String getSSID() { return isConnected() ? saved_ssid : String(); }

String getIPAddress() {
    if (!sta_netif) return "0.0.0.0";
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(sta_netif, &ip) != ESP_OK) return "0.0.0.0";
    char buf[16];
    snprintf(buf, sizeof(buf), IPSTR, IP2STR(&ip.ip));
    return String(buf);
}

int32_t getRSSI() {
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) return ap.rssi;
    return 0;
}

uint8_t getSignalPercent() {
    if (!isConnected()) return 0;
    int32_t rssi = getRSSI();
    if (rssi >= -30) return 100;
    if (rssi <= -90) return 0;
    return (uint8_t)(((rssi + 90) * 100) / 60);
}

void update() {
    if (!is_initialized || !wifi_enabled) return;
    last_update_time = millis();

    // Timeout detection for CONNECTING state (event handler handles CONNECTED/RECONNECTING)
    if (current_status == ConnectionStatus::CONNECTING && hasTimedOut()) {
        connection_start_time = 0;
        updateStatus(ConnectionStatus::FAILED);
        esp_wifi_disconnect();
        Serial.println("[WIFI_MGR] Connection timeout");
    } else if (current_status == ConnectionStatus::RECONNECTING && hasTimedOut()) {
        connection_start_time = 0;
        updateStatus(ConnectionStatus::DISCONNECTED);
        Serial.println("[WIFI_MGR] Reconnect timeout");
    }
}

void setAutoReconnect(bool enable) {
    auto_reconnect_enabled = enable;
    Serial.printf("[WIFI_MGR] Auto-reconnect: %s\n", enable ? "on" : "off");
}

bool getAutoReconnect() { return auto_reconnect_enabled; }

uint32_t getTimeSinceLastUpdate() { return millis() - last_update_time; }

void setEnabled(bool enabled) {
    if (!is_initialized || wifi_enabled == enabled) return;
    wifi_enabled = enabled;

    if (enabled) {
        wifi_ever_enabled = true;  // Session flag — NimBLE must not start after this
        // Create STA netif lazily — only when STA mode is actually activated.
        // Doing this in init() before AP mode sets STA state in the WiFi driver
        // that corrupts the AP WPA2 authenticator state machine.
        if (!sta_netif) {
            sta_netif = esp_netif_create_default_wifi_sta();
        }
        esp_wifi_set_mode(WIFI_MODE_STA);
        esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
        esp_wifi_start();
        updateStatus(ConnectionStatus::DISCONNECTED);
        Serial.println("[WIFI_MGR] WiFi enabled");
    } else {
        esp_wifi_disconnect();
        esp_wifi_stop();
        connection_start_time = 0;
        updateStatus(ConnectionStatus::DISCONNECTED);
        Serial.println("[WIFI_MGR] WiFi disabled");
    }
}

bool isEnabled() { return wifi_enabled; }
bool wasEverEnabled() { return wifi_ever_enabled; }

void ensureStaNetif() {
    if (!sta_netif) {
        sta_netif = esp_netif_create_default_wifi_sta();
    }
}

void printStatus() {
    Serial.printf("[WIFI_MGR] Status=%s, IP=%s, RSSI=%d dBm, AutoReconnect=%s\n",
                  getStatusString().c_str(), getIPAddress().c_str(),
                  getRSSI(), auto_reconnect_enabled ? "on" : "off");
}

} // namespace wifi_manager
