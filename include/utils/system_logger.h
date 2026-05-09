#ifndef SYSTEM_LOGGER_H
#define SYSTEM_LOGGER_H

#include "core/arduino_compat.h"

/**
 * @file system_logger.h
 * @brief Simplified system logger - non-blocking RAM buffer with periodic file flush
 *
 * Design principles:
 * - NEVER block on file I/O during log calls
 * - Buffer in RAM, flush periodically from background task
 * - Core dump handles crash forensics (separate from logging)
 * - Simple API: log(), logf(), flush()
 */

namespace system_logger {

// =============================================================================
// CONFIGURATION
// =============================================================================

constexpr const char* LOG_FILE = "/sdcard/logs/system.log";
constexpr size_t MAX_LOG_SIZE = 512 * 1024;     // 512KB max file size
constexpr size_t BUFFER_SIZE = 8192;            // 8KB RAM buffer
constexpr uint32_t FLUSH_INTERVAL_MS = 30000;   // Flush every 30 seconds

// =============================================================================
// LOG LEVELS
// =============================================================================

enum class Level {
    DEBUG,
    INFO,
    WARN,
    ERROR
};

// =============================================================================
// SIMPLE API
// =============================================================================

/**
 * @brief Initialize logger (call once at startup after SD card init)
 * @return true if successful
 */
bool init();

/**
 * @brief Log a message (non-blocking, writes to RAM buffer)
 * @param level Log level
 * @param tag Component tag (e.g., "GPS", "UI", "I2C")
 * @param message Log message
 */
void log(Level level, const char* tag, const char* message);

/**
 * @brief Log formatted message (non-blocking)
 * @param level Log level
 * @param tag Component tag
 * @param fmt Printf-style format string
 * @param ... Format arguments
 */
void logf(Level level, const char* tag, const char* fmt, ...);

/**
 * @brief Convenience functions
 */
inline void debug(const char* tag, const char* msg) { log(Level::DEBUG, tag, msg); }
inline void info(const char* tag, const char* msg)  { log(Level::INFO, tag, msg); }
inline void warn(const char* tag, const char* msg)  { log(Level::WARN, tag, msg); }
inline void error(const char* tag, const char* msg) { log(Level::ERROR, tag, msg); }

/**
 * @brief Flush buffer to file (call from System Task every 30s)
 * This is the ONLY function that does file I/O
 * @return true if flush successful
 */
bool flush();

/**
 * @brief Check if flush is due (call from System Task)
 * @return true if FLUSH_INTERVAL_MS has passed since last flush
 */
bool needsFlush();

/**
 * @brief Enable/disable logging
 */
void setEnabled(bool enabled);
bool isEnabled();

/**
 * @brief Get buffer usage for diagnostics
 * @return Bytes used in buffer (0 to BUFFER_SIZE)
 */
size_t getBufferUsage();

/**
 * @brief Get current log file size
 */
size_t getFileSize();

/**
 * @brief Close logger (flush and close file)
 */
void close();

} // namespace system_logger

#endif // SYSTEM_LOGGER_H
