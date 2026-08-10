#ifndef GPX_SERVER_H
#define GPX_SERVER_H

#include "core/arduino_compat.h"

namespace gpx_server {

/**
 * @brief Initialize WiFi HTTP server for GPX file uploads
 * @return true on success, false on failure
 */
bool init();

/**
 * @brief Handle incoming HTTP requests (call in main loop)
 */
void handle();

/**
 * @brief Start WiFi AP mode and HTTP server
 * @return true if server started successfully
 */
bool start();

/**
 * @brief Stop WiFi and HTTP server
 */
void stop();

/**
 * @brief Check if server is running
 * @return true if server is active
 */
bool isRunning();

/**
 * @brief Get server status information
 * @param ip_address Output buffer for IP address string
 * @param max_len Maximum length of output buffer
 * @return true if server is running and IP is available
 */
bool getStatus(char* ip_address, size_t max_len);

} // namespace gpx_server

#endif // GPX_SERVER_H
