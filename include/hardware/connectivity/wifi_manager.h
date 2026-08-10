#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include "core/arduino_compat.h"

/**
 * @file wifi_manager.h
 * @brief WiFi connection management with NVS credential storage
 *
 * This module provides WiFi connection handling for the radar display:
 * - Connect to WiFi networks with async status updates
 * - Auto-reconnect on disconnect
 * - Integration with settings_manager for credential persistence
 * - Connection status monitoring
 *
 * Usage:
 * 1. Call init() once at boot
 * 2. Use connect() to connect to a network (saves credentials automatically)
 * 3. Use autoConnect() to connect using saved credentials
 * 4. Poll getConnectionStatus() to monitor connection state
 * 5. Use disconnect() to disconnect and optionally forget network
 *
 * Architecture:
 * - Non-blocking connection attempts with timeout
 * - Status callbacks for UI updates (future enhancement)
 * - Automatic reconnection on WiFi loss
 */

namespace wifi_manager {

/**
 * @brief WiFi connection status
 */
enum class ConnectionStatus {
    DISCONNECTED,      // Not connected
    CONNECTING,        // Connection in progress
    CONNECTED,         // Successfully connected
    FAILED,            // Connection failed (wrong password, timeout, etc.)
    RECONNECTING       // Auto-reconnect in progress
};

/**
 * @brief Initialize WiFi manager
 * @return true if initialization successful
 *
 * Must be called before any other wifi_manager functions.
 * Sets WiFi mode to STA (station) and configures auto-reconnect.
 */
bool init();

/**
 * @brief Connect to WiFi network
 * @param ssid Network SSID
 * @param password Network password (empty string for open networks)
 * @param timeout_ms Maximum time to wait for connection (default: 15000ms = 15s)
 * @return true if connection initiated successfully, false on error
 *
 * This function:
 * - Saves credentials to NVS via settings_manager
 * - Initiates connection (non-blocking)
 * - Sets status to CONNECTING
 * - Returns immediately (use getConnectionStatus() to poll)
 *
 * Note: Use isConnected() or getConnectionStatus() to check final result
 */
bool connect(const String& ssid, const String& password, uint32_t timeout_ms = 15000);

/**
 * @brief Connect using saved credentials from NVS
 * @param timeout_ms Maximum time to wait for connection (default: 15000ms = 15s)
 * @return true if connection initiated (credentials found), false if no saved credentials
 *
 * Attempts to load credentials from settings_manager and connect.
 * Useful for auto-connect on boot.
 */
bool autoConnect(uint32_t timeout_ms = 15000);

/**
 * @brief Disconnect from WiFi
 * @param forget If true, also clear saved credentials from NVS
 * @return true if disconnect successful
 *
 * Use forget=true for "Forget Network" functionality
 */
bool disconnect(bool forget = false);

/**
 * @brief Check if WiFi is connected
 * @return true if connected and has IP address
 */
bool isConnected();

/**
 * @brief Get detailed connection status
 * @return Current ConnectionStatus enum value
 */
ConnectionStatus getConnectionStatus();

/**
 * @brief Get connection status as human-readable string
 * @return Status string (e.g., "Connected", "Connecting...", "Disconnected")
 */
String getStatusString();

/**
 * @brief Get current SSID
 * @return Connected SSID, or empty string if not connected
 */
String getSSID();

/**
 * @brief Get current IP address
 * @return IP address as string (e.g., "192.168.1.100"), or "0.0.0.0" if not connected
 */
String getIPAddress();

/**
 * @brief Get signal strength (RSSI)
 * @return RSSI in dBm (e.g., -65), or 0 if not connected
 */
int32_t getRSSI();

/**
 * @brief Get signal strength as percentage
 * @return Signal strength 0-100% (0 = no signal, 100 = excellent)
 */
uint8_t getSignalPercent();

/**
 * @brief Update connection state machine (call periodically)
 *
 * This function should be called regularly (e.g., every 1000ms) to:
 * - Check for connection timeouts
 * - Handle auto-reconnect
 * - Update connection status
 *
 * Can be called from LVGL timer, loop(), or dedicated task
 */
void update();

/**
 * @brief Enable/disable auto-reconnect
 * @param enable true to enable auto-reconnect on disconnect
 *
 * Default: enabled
 * When enabled, WiFi will automatically reconnect if connection is lost
 */
void setAutoReconnect(bool enable);

/**
 * @brief Check if auto-reconnect is enabled
 * @return true if auto-reconnect is enabled
 */
bool getAutoReconnect();

/**
 * @brief Enable/disable WiFi completely
 * @param enabled true to enable WiFi, false to disable
 *
 * When disabled:
 * - Disconnects from any current network
 * - Powers down WiFi radio (saves ~80mA)
 * - Prevents auto-reconnect
 * - Scanning and connections are blocked until re-enabled
 *
 * When re-enabled:
 * - Powers up WiFi radio
 * - Does NOT auto-connect (user must manually connect or call autoConnect())
 *
 * Power savings: ~80-120mA when disabled
 */
void setEnabled(bool enabled);

/**
 * @brief Check if WiFi is enabled
 * @return true if WiFi is enabled (radio powered on)
 */
bool isEnabled();

/**
 * @brief Check if WiFi STA was ever activated this boot session.
 * @return true if setEnabled(true) was called at any point since boot.
 *
 * NimBLE cannot be safely initialized after WiFi STA has run because
 * esp_wifi_stop() does not free DMA-capable SRAM, leaving the heap
 * too fragmented for NimBLE's BT controller allocation. Use this flag
 * instead of isEnabled() to permanently block beacon proximity for the
 * rest of the session once WiFi has been used.
 */
bool wasEverEnabled();

/**
 * @brief Get time since last connection check (milliseconds)
 * @return Milliseconds since last update() call
 *
 * For diagnostics - helps identify if update() is being called regularly
 */
uint32_t getTimeSinceLastUpdate();

/**
 * @brief Print WiFi status to Serial (for debugging)
 *
 * Shows:
 * - Connection status
 * - SSID and IP (if connected)
 * - Signal strength
 * - Auto-reconnect state
 */
void printStatus();

/**
 * @brief Ensure the STA netif exists (creates it if not yet created).
 * Safe to call multiple times. Used by AP mode (APSTA) to ensure both
 * netifs exist without double-creating the STA netif.
 */
void ensureStaNetif();

} // namespace wifi_manager

#endif // WIFI_MANAGER_H
