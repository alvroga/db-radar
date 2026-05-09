#include "diagnostics.h"
#include "system_config.h"
#include "scanner.h"
#include "ui_manager.h"
#include "memory_manager.h"
#include "task_manager.h"
#include "device_manager.h"
#include "system_logger.h"
#include "ntp_sync.h"
#include "rtc_pcf85063.h"
#include "settings_manager.h"
#include "gps_bh880.h"
#include "navigation/gps_quality.h"
#include "i2c_manager.h"
#include "gpx/gpx_server.h"
#include "wifi_manager.h"
#include "hardware/sensors/battery.h"
#include "hardware/connectivity/beacon_proximity.h"
#include "hardware/sensors/compass_qmc5883l.h"
#include "core/arduino_compat.h"
#include "esp_core_dump.h"
#include <esp_system.h>
#include "esp_wifi.h"

namespace diagnostics {

// Forward declarations
static void handleGPSCommand(const char* args);
static void handleGPXCommand(const char* args);
static void handleBatteryCommand(const char* args);
static void handleCrashCommand(const char* args);
static void handleLogCommand(const char* args);
static void handleNTPCommand(const char* args);
static void handleDevCommand(const char* args);
static void handleBeaconCommand(const char* args);
static void handleCompassCommand(const char* args);
static void handleAPToggle(bool enable);

// Global state
static Config g_config;
static DiagState g_diag_state;

DiagState& getDiagState() {
    return g_diag_state;
}

bool init(const Config& config) {
    g_config = config;

    // Initialize diagnostic state
    g_diag_state.lvgl_freeze = false;
    g_diag_state.lvgl_freeze_until_ms = 0;
    g_diag_state.cmd_length = 0;

    Serial.println("[DIAGNOSTICS] Initialized successfully");
    Serial.println("Type 'help' for available commands");

    return true;
}

void processCommands() {
    // Non-blocking serial command processing
    while (Serial.available() > 0) {
        char c = (char)Serial.read();

        if (c == '\n' || c == '\r') {
            if (g_diag_state.cmd_length > 0) {
                g_diag_state.cmd_buffer[g_diag_state.cmd_length] = '\0';
                parseCommand(g_diag_state.cmd_buffer);
                g_diag_state.cmd_length = 0;
            }
        } else if (g_diag_state.cmd_length < sizeof(g_diag_state.cmd_buffer) - 1) {
            g_diag_state.cmd_buffer[g_diag_state.cmd_length++] = c;
        } else {
            // Overflow: reset buffer
            g_diag_state.cmd_length = 0;
        }
    }

    // Update LVGL freeze state
    updateLVGLFreezeState();
}

void parseCommand(const char* command) {
    // Skip leading whitespace
    const char* p = command;
    while (*p == ' ') p++;

    if (strncmp(p, "help", 4) == 0) {
        handleHelpCommand();
    } else if (strncmp(p, "diag", 4) == 0) {
        handleDiagCommand(p + 4);
    } else if (strncmp(p, "gps", 3) == 0) {
        handleGPSCommand(p + 3);
    } else if (strncmp(p, "gpx", 3) == 0) {
        handleGPXCommand(p + 3);
    } else if (strncmp(p, "memory", 6) == 0) {
        handleMemoryCommand(p + 6);
    } else if (strncmp(p, "mem", 3) == 0) {
        handleMemoryCommand(p + 3);
    } else if (strncmp(p, "config", 6) == 0) {
        handleConfigCommand(p + 6);
    } else if (strncmp(p, "task", 4) == 0) {
        handleTaskCommand(p + 4);
    } else if (strncmp(p, "battery", 7) == 0) {
        handleBatteryCommand(p + 7);
    } else if (strncmp(p, "bat", 3) == 0) {
        handleBatteryCommand(p + 3);
    } else if (strncmp(p, "crash", 5) == 0) {
        handleCrashCommand(p + 5);
    } else if (strncmp(p, "log", 3) == 0) {
        handleLogCommand(p + 3);
    } else if (strncmp(p, "ntp", 3) == 0) {
        handleNTPCommand(p + 3);
    } else if (strncmp(p, "dev", 3) == 0) {
        handleDevCommand(p + 3);
    } else if (strncmp(p, "compass", 7) == 0) {
        handleCompassCommand(p + 7);
    } else if (strncmp(p, "beacon", 6) == 0) {
        handleBeaconCommand(p + 6);
    } else if (strncmp(p, "version", 7) == 0) {
        Serial.printf("DRAC OS %s\n", FW_VERSION);
    } else {
        Serial.printf("Unknown command: %s\n", command);
        Serial.println("Type 'help' for available commands");
    }
}

void handleHelpCommand() {
    printAvailableCommands();
}

void handleDiagCommand(const char* args) {
    // Skip whitespace
    while (*args == ' ') args++;

    if (strncmp(args, "wifi", 4) == 0) {
        args += 4;
        while (*args == ' ') args++;
        bool on = (strncmp(args, "on", 2) == 0);
        bool off = (strncmp(args, "off", 3) == 0);
        if (on || off) {
            handleWiFiToggle(on);
        } else {
            Serial.println("Usage: diag wifi on|off");
        }
    } else if (strncmp(args, "ble", 3) == 0) {
        args += 3;
        while (*args == ' ') args++;
        bool on = (strncmp(args, "on", 2) == 0);
        bool off = (strncmp(args, "off", 3) == 0);
        if (on || off) {
            handleBLEToggle(on);
        } else {
            Serial.println("Usage: diag ble on|off");
        }
    } else if (strncmp(args, "ap", 2) == 0) {
        args += 2;
        while (*args == ' ') args++;
        bool on = (strncmp(args, "on", 2) == 0);
        bool off = (strncmp(args, "off", 3) == 0);
        if (on || off) {
            handleAPToggle(on);
        } else {
            Serial.println("Usage: diag ap on|off");
        }
    } else if (strncmp(args, "i2c", 3) == 0) {
        i2c_manager::scanBus();
    } else if (strncmp(args, "freeze", 6) == 0 || strncmp(args, "lvgl_freeze", 11) == 0) {
        // Skip past the command name
        if (args[0] == 'f') args += 6; else args += 11;
        while (*args == ' ') args++;
        bool on = (strncmp(args, "on", 2) == 0);
        bool off = (strncmp(args, "off", 3) == 0);
        if (on || off) {
            handleLVGLFreezeToggle(on);
        } else {
            Serial.println("Usage: diag freeze on|off  or  diag lvgl_freeze on|off");
        }
    } else {
        Serial.println("Unknown diagnostic command");
        printAvailableCommands();
    }
}

void handleWiFiToggle(bool enable) {
    scanner::setWiFiEnabled(enable);
    Serial.printf("[DIAG] WiFi scanning %s\n", enable ? "enabled" : "disabled");
}

void handleBLEToggle(bool enable) {
    // General BLE scanning is handled by beacon_proximity.cpp (zoom-gated), not scanner module
    Serial.printf("[DIAG] BLE is managed by beacon_proximity (zoom-gated); use beacon settings to configure\n");
    (void)enable;
}

void handleAPToggle(bool enable) {
    if (enable) {
        Serial.println("[DIAG] Starting Access Point mode...");

        // Stop WiFi station mode if active
        if (wifi_manager::isConnected()) {
            Serial.println("[DIAG] ⚠️ Disconnecting from WiFi (AP/WiFi mutually exclusive)");
            wifi_manager::setEnabled(false);
        }

        if (gpx_server::start()) {
            char ip[20] = {};
            gpx_server::getStatus(ip, sizeof(ip));
            Serial.println("[DIAG] ✓ AP created and web server started");
            Serial.printf("[DIAG]   SSID: Radar-GPX\n");
            Serial.printf("[DIAG]   Password: radar123\n");
            Serial.printf("[DIAG]   Web portal ready at: http://%s\n", ip);
        } else {
            Serial.println("[DIAG] ✗ Failed to start AP/web server");
        }
    } else {
        Serial.println("[DIAG] Stopping Access Point mode...");

        // Stop GPX server
        if (gpx_server::isRunning()) {
            gpx_server::stop();
            Serial.println("[DIAG] ✓ Web server stopped");
        }

        Serial.println("[DIAG] ✓ AP disabled");
    }
}

void handleLVGLFreezeToggle(bool enable) {
    if (enable) {
        g_diag_state.lvgl_freeze = true;
        g_diag_state.lvgl_freeze_until_ms = millis() + g_config.freeze_duration_ms;
        Serial.printf("[DIAG] LVGL freeze enabled for %u ms\n", (unsigned)g_config.freeze_duration_ms);
    } else {
        g_diag_state.lvgl_freeze = false;
        g_diag_state.lvgl_freeze_until_ms = 0;
        Serial.println("[DIAG] LVGL freeze disabled");
    }
}

void handleGPSCommand(const char* args) {
    // Skip whitespace
    while (*args == ' ') args++;

    if (strncmp(args, "status", 6) == 0 || strlen(args) == 0) {
        const device_manager::DeviceState& dev = device_manager::getDeviceState();

        Serial.println("==== GPS Status ====");
        Serial.printf("GPS Module:      %s\n", dev.gps_ok ? "Initialized" : "Not initialized");

        if (dev.last_gps_data.valid) {
            Serial.printf("Fix Status:      VALID\n");
            Serial.printf("Latitude:        %.6f°\n", dev.last_gps_data.lat);
            Serial.printf("Longitude:       %.6f°\n", dev.last_gps_data.lon);
            Serial.printf("Altitude:        %.1f m\n", dev.last_gps_data.alt);
            Serial.printf("Satellites:      %d\n", dev.last_gps_data.sats);
            Serial.printf("HDOP:            %.1f\n", dev.last_gps_data.hdop);

            if (dev.last_gps_data.hasTime) {
                Serial.printf("UTC Time:        %04d-%02d-%02d %02d:%02d:%02d\n",
                              dev.last_gps_data.year, dev.last_gps_data.month,
                              dev.last_gps_data.day, dev.last_gps_data.hour,
                              dev.last_gps_data.minute, dev.last_gps_data.second);
            }
        } else {
            Serial.printf("Fix Status:      SEARCHING\n");
            Serial.printf("Satellites:      %d (acquiring...)\n", dev.last_gps_data.sats);
            Serial.println("\nTroubleshooting:");
            Serial.println("  - Module needs OUTDOOR clear sky view");
            Serial.println("  - PPS LED flashing = normal (searching)");
            Serial.println("  - First fix can take 26-30 seconds");
            Serial.println("  - Check antenna connection");
        }

        Serial.println("====================");
    }
    else if (strncmp(args, "quality", 7) == 0) {
        // GPS Quality Report (Phase 1.3)
        gps_quality::printReport();
    }
    else if (strncmp(args, "raw", 3) == 0) {
        args += 3;
        while (*args == ' ') args++;
        uint32_t duration = 5000;
        if (*args) duration = atoi(args) * 1000;  // seconds to ms
        if (duration < 1000) duration = 5000;
        if (duration > 30000) duration = 30000;
        gps_bh880::dumpRaw(duration);
    }
    else if (strncmp(args, "config", 6) == 0) {
        args += 6;
        while (*args == ' ') args++;

        if (strncmp(args, "baud", 4) == 0) {
            args += 4;
            while (*args == ' ') args++;
            uint32_t baud = atoi(args);
            if (gps_bh880::setBaudrate(baud)) {
                Serial.printf("[GPS] Baudrate set to %u\n", baud);
                Serial.println("[GPS] WARNING: Serial connection will break!");
                Serial.println("[GPS] You must restart the GPS serial port manually");
            } else {
                Serial.println("Usage: gps config baud <rate>");
                Serial.println("Valid rates: 9600, 115200, 230400, 460800, 921600");
            }
        }
        else {
            Serial.println("GPS configuration commands:");
            Serial.println("  gps config baud <rate>    - Change baudrate");
            Serial.println("Note: BH-880 (M10) ignores legacy CFG-RATE. Rate fixed at 10Hz.");
        }
    }
    else if (strncmp(args, "restart", 7) == 0) {
        args += 7;
        while (*args == ' ') args++;

        if (strncmp(args, "hot", 3) == 0) {
            if (gps_bh880::hotStart()) {
                Serial.println("[GPS] Hot start initiated (using last position & time)");
            }
        } else if (strncmp(args, "warm", 4) == 0) {
            if (gps_bh880::warmStart()) {
                Serial.println("[GPS] Warm start initiated (using last position)");
            }
        } else if (strncmp(args, "cold", 4) == 0) {
            if (gps_bh880::coldStart()) {
                Serial.println("[GPS] Cold start initiated (full satellite search)");
            }
        } else {
            Serial.println("Usage: gps restart <hot|warm|cold>");
            Serial.println("  hot  - Use last known position and time (fastest)");
            Serial.println("  warm - Use last known position");
            Serial.println("  cold - Full satellite search");
        }
    }
    else if (strncmp(args, "info", 4) == 0) {
        gps_bh880::printModuleInfo();
    }
    else if (strncmp(args, "ping", 4) == 0) {
        gps_bh880::ping(1000);
    }
    else if (strncmp(args, "reset", 5) == 0) {
        Serial.println("[GPS] WARNING: This will erase all saved settings!");
        Serial.println("[GPS] Factory reset in 3 seconds... (power cycle to abort)");
        delay(3000);
        if (gps_bh880::factoryReset()) {
            Serial.println("[GPS] Factory reset complete - all settings cleared");
        }
    }
    else if (strncmp(args, "power", 5) == 0) {
        args += 5;
        while (*args == ' ') args++;
        if (strncmp(args, "full", 4) == 0) {
            Serial.println("[GPS] Setting Full Power mode (0x00)...");
            if (gps_bh880::setPowerMode(0x00)) {
                Serial.println("[GPS] CFG-PMS: Full Power — ACK OK");
            } else {
                Serial.println("[GPS] CFG-PMS: Full Power — ACK timeout (command may still apply)");
            }
        } else if (strncmp(args, "agg1", 4) == 0) {
            Serial.println("[GPS] Setting Aggressive 1Hz mode (0x02)...");
            if (gps_bh880::setPowerMode(0x02)) {
                Serial.println("[GPS] CFG-PMS: Aggressive 1Hz — ACK OK");
            } else {
                Serial.println("[GPS] CFG-PMS: Aggressive 1Hz — ACK timeout (command may still apply)");
            }
        } else if (strncmp(args, "interval", 8) == 0) {
            args += 8;
            while (*args == ' ') args++;
            uint32_t period = 10000, ontime = 3000;
            if (*args) {
                period = atoi(args);
                const char* sp = strchr(args, ' ');
                if (sp) ontime = atoi(sp + 1);
            }
            Serial.printf("[GPS] Setting Interval mode: period=%ums, onTime=%ums...\n", period, ontime);
            Serial.println("[GPS] WARNING: M10 ACKs but ignores interval mode - use agg1 for actual power saving");
            if (gps_bh880::setPowerMode(0x01, period, ontime)) {
                Serial.printf("[GPS] CFG-PMS: Interval %ums/%ums — ACK OK (but M10 ignores this mode)\n", ontime, period);
            } else {
                Serial.printf("[GPS] CFG-PMS: Interval %ums/%ums — ACK timeout\n", ontime, period);
            }
        } else {
            Serial.println("Usage: gps power <mode>");
            Serial.println("  gps power full                       - Full power (10Hz, normal)");
            Serial.println("  gps power agg1                       - Aggressive 1Hz (low power)");
            Serial.println("  gps power interval [period_ms] [on_ms] - Duty-cycle (default 10000/3000)");
        }
    }
    else {
        Serial.println("Available GPS commands (BH-880 / UBX protocol):");
        Serial.println("  gps info               - Print chip SW/HW version (identifies actual chip)");
        Serial.println("  gps ping               - Test TX/RX wiring (no fix needed)");
        Serial.println("  gps status             - Show GPS fix status and coordinates");
        Serial.println("  gps quality            - Show detailed quality report");
        Serial.println("  gps raw [seconds]      - Dump raw UBX hex data (default 5s, max 30s)");
        Serial.println("  gps power full         - Full power mode");
        Serial.println("  gps power agg1         - Aggressive 1Hz low-power mode");
        Serial.println("  gps power interval [period_ms] [on_ms] - Duty-cycle mode");
        Serial.println("  gps config baud <rate> - Change baudrate (9600-921600)");
        Serial.println("  gps restart <hot|warm|cold> - Restart GPS module");
        Serial.println("  gps reset              - Factory reset (clears all settings)");
    }
}

bool shouldSkipLVGLFlush() {
    if (g_diag_state.lvgl_freeze) {
        uint32_t now = millis();
        if (now <= g_diag_state.lvgl_freeze_until_ms) {
            return true; // Skip flush during freeze window
        } else {
            // Freeze window expired; clear flag
            g_diag_state.lvgl_freeze = false;
            return false;
        }
    }
    return false;
}

void updateLVGLFreezeState() {
    if (g_diag_state.lvgl_freeze) {
        uint32_t now = millis();
        if (now > g_diag_state.lvgl_freeze_until_ms) {
            g_diag_state.lvgl_freeze = false;
        }
    }
}

void handleMemoryCommand(const char* args) {
    // Skip whitespace
    while (*args == ' ') args++;

    if (strncmp(args, "stats", 5) == 0) {
        memory_manager::logCurrentStats();
    } else if (strncmp(args, "info", 4) == 0) {
        memory_manager::logMemoryInfo();
    } else if (strncmp(args, "report", 6) == 0) {
        memory_manager::generateMemoryReport();
    } else if (strncmp(args, "pools", 5) == 0) {
        memory_manager::pools::logPoolStats();
        Serial.println("[MEMORY] Phase 2: Static memory pools active (crash-safe)");
    } else if (strncmp(args, "cleanup", 7) == 0) {
        args += 7;
        while (*args == ' ') args++;
        if (strncmp(args, "screens", 7) == 0) {
            ui_manager::cleanupAllScreens();
            Serial.println("[MEMORY] UI screens cleanup completed");
        } else if (strncmp(args, "lvgl", 4) == 0) {
            memory_manager::cleanup::cleanupLVGLObjects();
            Serial.println("[MEMORY] LVGL cleanup completed");
        } else if (strlen(args) == 0) {
            // Default cleanup - both screens and LVGL
            ui_manager::cleanupAllScreens();
            memory_manager::cleanup::cleanupLVGLObjects();
            Serial.println("[MEMORY] Full cleanup completed");
        } else {
            Serial.println("Usage: memory cleanup [screens|lvgl]");
        }
    } else if (strncmp(args, "integrity", 9) == 0) {
        bool ok = memory_manager::checkHeapIntegrity();
        Serial.printf("[MEMORY] Heap integrity check: %s\n", ok ? "PASS" : "FAIL");
    } else if (strncmp(args, "leak", 4) == 0) {
        args += 4;
        while (*args == ' ') args++;
        if (strncmp(args, "start", 5) == 0) {
            memory_manager::cleanup::startLeakDetection();
        } else if (strncmp(args, "stop", 4) == 0) {
            memory_manager::cleanup::stopLeakDetection();
        } else if (strncmp(args, "report", 6) == 0) {
            memory_manager::cleanup::reportLeaks();
        } else {
            Serial.println("Usage: memory leak start|stop|report");
        }
    } else if (strncmp(args, "stress", 6) == 0) {
        Serial.println("[MEMORY] Phase 2: Memory stress test with static pools");

        memory_manager::logCurrentStats();
        memory_manager::pools::logPoolStats();

        // Test 1: Pool allocation cycling
        Serial.println("[MEMORY] Test 1: Pool allocation cycling");
        void* small_ptrs[4]; // Test 4 allocations
        void* string_ptrs[2]; // Test 2 string allocations

        for (int cycle = 0; cycle < 3; cycle++) {
            Serial.printf("  Cycle %d: Allocating from pools...\n", cycle + 1);

            // Allocate from small pool
            for (int i = 0; i < 4; i++) {
                small_ptrs[i] = memory_manager::pools::allocSmall(100);
            }

            // Allocate from string pool
            for (int i = 0; i < 2; i++) {
                string_ptrs[i] = memory_manager::pools::allocString(64);
            }

            memory_manager::pools::logPoolStats();

            // Free all allocations
            for (int i = 0; i < 4; i++) {
                memory_manager::pools::freeSmall(small_ptrs[i]);
            }
            for (int i = 0; i < 2; i++) {
                memory_manager::pools::freeString(string_ptrs[i]);
            }

            Serial.printf("  Cycle %d: Freed all pool allocations\n", cycle + 1);
        }

        // Test 2: UI screen cycling (simplified for radar project)
        Serial.println("[MEMORY] Test 2: Skipped (radar uses single screen)");
        memory_manager::updateStats();
        Serial.printf("  heap=%u, psram=%u\n",
                      memory_manager::getFreeHeap(), memory_manager::getFreePSRAM());

        // Final check
        bool integrity_ok = memory_manager::checkHeapIntegrity();
        memory_manager::logCurrentStats();
        memory_manager::pools::logPoolStats();
        Serial.printf("[MEMORY] Stress test completed. Integrity: %s\n",
                      integrity_ok ? "PASS" : "FAIL");

    } else if (strlen(args) == 0) {
        // Default to stats if no args
        memory_manager::logCurrentStats();
    } else {
        Serial.println("Unknown memory command");
        Serial.println("Available memory commands (Phase 2 - Static Memory Pools):");
        Serial.println("  memory stats         - Show current memory statistics");
        Serial.println("  memory info          - Show memory layout information");
        Serial.println("  memory report        - Generate memory report");
        Serial.println("  memory pools         - Show static pool usage");
        Serial.println("  memory integrity     - Check heap integrity");
        Serial.println("  memory stress        - Run memory stress test with pools");
        Serial.println("  memory cleanup screens - Clean up UI screens");
        Serial.println("  memory leak start/stop/report - Leak detection");
        Serial.println("Note: Static memory pools active (crash-safe)");
    }
}

void handleConfigCommand(const char* args) {
    // Skip whitespace
    while (*args == ' ') args++;

    if (strncmp(args, "show", 4) == 0) {
        showCurrentConfiguration();
    } else if (strncmp(args, "set", 3) == 0) {
        args += 3;
        while (*args == ' ') args++;
        handleConfigSet(args);
    } else if (strncmp(args, "timing", 6) == 0) {
        showTimingConfiguration();
    } else if (strncmp(args, "display", 7) == 0) {
        showDisplayConfiguration();
    } else if (strncmp(args, "pins", 4) == 0) {
        showPinConfiguration();
    } else if (strlen(args) == 0) {
        showCurrentConfiguration();
    } else {
        Serial.println("Unknown config command");
        Serial.println("Available config commands:");
        Serial.println("  config show          - Show all configuration values");
        Serial.println("  config display       - Show display configuration");
        Serial.println("  config timing        - Show timing configuration");
        Serial.println("  config pins          - Show pin configuration");
        Serial.println("  config set <param> <value> - Set configuration parameter");
        Serial.println("");
        Serial.println("Settable parameters:");
        Serial.println("  wifi_interval <ms>   - WiFi scan interval");
        Serial.println("  ble_interval <ms>    - BLE scan interval");
        Serial.println("  rtc_interval <ms>    - RTC read interval");
        Serial.println("  brightness <0-100>   - Display brightness");
    }
}

void showCurrentConfiguration() {
    Serial.println("==== System Configuration ====");

    // Hardware identification
    Serial.printf("Hardware:        %s\n", system_config::HARDWARE_NAME);
    Serial.printf("Description:     %s\n", system_config::HARDWARE_DESCRIPTION);

    showDisplayConfiguration();
    showTimingConfiguration();
    showPinConfiguration();

    Serial.println("===============================");
}

void showDisplayConfiguration() {
    Serial.println("---- Display Configuration ----");
    Serial.printf("Screen Size:     %dx%d\n",
                  system_config::display::SCREEN_WIDTH,
                  system_config::display::SCREEN_HEIGHT);
    Serial.printf("Pixel Clock:     %d Hz\n", system_config::display::PCLK_HZ);
    Serial.printf("Buffer Lines:    %d\n", system_config::display::BUFFER_LINES);
    Serial.printf("Header Height:   %d px\n", system_config::ui::HEADER_HEIGHT);
    Serial.printf("Safe Margin:     %d px\n", system_config::ui::SAFE_MARGIN);
    Serial.printf("PWM Frequency:   %d Hz\n", system_config::backlight::PWM_FREQ_HZ);
}

void showTimingConfiguration() {
    Serial.println("---- Timing Configuration ----");
    Serial.printf("WiFi Scan:       %d ms\n", system_config::timing::WIFI_SCAN_INTERVAL_MS);
    Serial.printf("BLE Scan:        %d ms\n", system_config::timing::BLE_SCAN_INTERVAL_MS);
    Serial.printf("BLE Duration:    %d ms\n", system_config::timing::BLE_SCAN_DURATION_MS);
    Serial.printf("RTC Read:        %d ms\n", system_config::timing::RTC_READ_INTERVAL_MS);
    Serial.printf("Memory Check:    %d ms\n", system_config::timing::MEMORY_CHECK_INTERVAL_MS);
    Serial.printf("Health Check:    %d ms\n", system_config::timing::HEALTH_CHECK_INTERVAL_MS);
}

void showPinConfiguration() {
    Serial.println("---- Pin Configuration ----");
    Serial.printf("LED:             %d\n", system_config::pins::LED);
    Serial.printf("I2C SDA:         %d\n", system_config::pins::I2C_SDA);
    Serial.printf("I2C SCL:         %d\n", system_config::pins::I2C_SCL);
    Serial.printf("LCD Backlight:   %d\n", system_config::pins::LCD_BL);
    Serial.printf("LCD PCLK:        %d\n", system_config::pins::LCD_PCLK);
    Serial.printf("LCD DE:          %d\n", system_config::pins::LCD_DE);
    Serial.printf("LCD VSYNC:       %d\n", system_config::pins::LCD_VSYNC);
    Serial.printf("LCD HSYNC:       %d\n", system_config::pins::LCD_HSYNC);
    Serial.printf("GPS TX:          %d\n", system_config::pins::GPS_TX);
    Serial.printf("GPS RX:          %d\n", system_config::pins::GPS_RX);
}

void handleConfigSet(const char* args) {
    Serial.println("[CONFIG] Runtime configuration changes not implemented yet");
    Serial.println("[CONFIG] Configuration values are compile-time constants");
    Serial.println("[CONFIG] To change values, modify system_config.h and recompile");
    Serial.println("");
    Serial.println("Future enhancement: Save configuration to SD card for runtime changes");
}

void printAvailableCommands() {
    Serial.println("Available commands:");
    Serial.println("  help                 - Show this help");
    Serial.println("  version              - Show firmware version");
    Serial.println("");
    Serial.println("GPS Commands (BH-880 / UBX):");
    Serial.println("  gps info             - Print chip SW/HW version (identifies actual chip)");
    Serial.println("  gps ping             - Test TX/RX wiring (no fix needed)");
    Serial.println("  gps status           - Show GPS fix status and coordinates");
    Serial.println("  gps quality          - Show detailed quality report");
    Serial.println("  gps raw [seconds]    - Dump raw UBX hex data (default 5s)");
    Serial.println("  gps power full       - Full power mode (10Hz normal)");
    Serial.println("  gps power agg1       - Aggressive 1Hz low-power mode");
    Serial.println("  gps power interval [period_ms] [on_ms] - Duty-cycle mode");
    Serial.println("  gps config baud <..> - Change baudrate");
    Serial.println("  gps restart <mode>   - Restart GPS (hot/warm/cold)");
    Serial.println("  gps reset            - Factory reset GPS module");
    Serial.println("");
    Serial.println("GPX Web Server Commands:");
    Serial.println("  gpx status           - Show GPX server status");
    Serial.println("  gpx ap               - Switch to AP mode (test network issue)");
    Serial.println("  gpx sta              - Switch back to Station mode");
    Serial.println("  gpx restart          - Restart GPX server");
    Serial.println("");
    Serial.println("Diagnostic Commands:");
    Serial.println("  diag i2c             - Scan I2C bus for connected devices");
    Serial.println("  diag wifi on|off     - Enable/disable WiFi scanning");
    Serial.println("  diag ble on|off      - Enable/disable Bluetooth scanning");
    Serial.println("  diag ap on|off       - Enable/disable Access Point mode");
    Serial.println("  diag freeze on|off   - Freeze/unfreeze LVGL display");
    Serial.println("");
    Serial.println("Task Commands:");
    Serial.println("  task status          - Show FreeRTOS task status and health");
    Serial.println("  task stats           - Show task performance statistics");
    Serial.println("  task enable/disable  - Switch between task and loop mode");
    Serial.println("");
    Serial.println("Memory Commands:");
    Serial.println("  memory [stats]       - Show memory statistics");
    Serial.println("  memory info          - Show memory layout info");
    Serial.println("  memory report        - Generate memory report");
    Serial.println("  memory pools         - Show static pool usage");
    Serial.println("  memory cleanup       - Force cleanup");
    Serial.println("  memory integrity     - Check heap integrity");
    Serial.println("  memory leak <cmd>    - Leak detection commands");
    Serial.println("");
    Serial.println("Configuration Commands:");
    Serial.println("  config show          - Show current configuration values");
    Serial.println("  config set <param> <value> - Set configuration parameter");
    Serial.println("");
    Serial.println("Battery Commands:");
    Serial.println("  battery status       - Show complete battery status (voltage, state, %)");
    Serial.println("  battery voltage      - Show battery voltage");
    Serial.println("  battery percent      - Show battery percentage");
    Serial.println("  battery charging     - Check if battery is charging");
    Serial.println("  battery state        - Show battery state (charging/discharging/full/stable)");
    Serial.println("  battery raw          - Show raw ADC value");
    Serial.println("  battery info         - Show hardware configuration");
    Serial.println("  battery monitor on|off - Enable/disable periodic logging");
    Serial.println("");
    Serial.println("Crash Logging Commands:");
    Serial.println("  crash dump           - Show last crash dump information");
    Serial.println("  crash clear          - Clear crash dump data");
    Serial.println("  crash info           - Show crash logging system info");
    Serial.println("");
    Serial.println("System Logger Commands:");
    Serial.println("  log status           - Show logger status and statistics");
    Serial.println("  log list             - List all log files with sizes");
    Serial.println("  log rotate           - Force log rotation to next file");
    Serial.println("  log size             - Show total size of all logs");
    Serial.println("  log enable|disable   - Enable/disable logging");
    Serial.println("");
    Serial.println("NTP Sync Commands:");
    Serial.println("  ntp status           - Show NTP sync status");
    Serial.println("  ntp sync             - Force NTP time sync");
    Serial.println("  ntp settime YYYY-MM-DD HH:MM:SS - Manually set RTC time");
    Serial.println("  ntp timezone <gmt> [dst] - Set timezone (e.g., ntp timezone -5 1)");
    Serial.println("");
    Serial.println("Compass Commands (QMC5883L on BH-880):");
    Serial.println("  compass [status]     - Show chip ID and configuration");
    Serial.println("  compass init         - Initialize sensor (continuous mode)");
    Serial.println("  compass read         - Read raw X/Y/Z and compute heading");
    Serial.println("  compass stream [s]   - Stream readings for N seconds");
    Serial.println("");
    Serial.println("Developer Commands:");
    Serial.println("  dev on|off           - Enable/disable dev mode (persists)");
    Serial.println("  dev show|hide        - Show/hide DEV tab in settings");
    Serial.println("  dev status           - Show current dev mode status");
}

void handleTaskCommand(const char* args) {
    // Skip whitespace
    while (*args == ' ') args++;

    if (strncmp(args, "status", 6) == 0) {
        if (task_manager::isTaskModeActive()) {
            Serial.println("==== FreeRTOS Task Status ====");
            task_manager::printTaskStatus();
        } else {
            Serial.println("[TASK] Tasks not active - running in legacy loop mode");
            Serial.println("Use 'task enable' to switch to task mode");
        }
    } else if (strncmp(args, "stats", 5) == 0) {
        if (task_manager::isTaskModeActive()) {
            task_manager::SystemStats stats = task_manager::getSystemStats();
            Serial.println("==== Task Statistics ====");
            Serial.printf("System Uptime: %lu ms\n", stats.uptime_ms);
            Serial.printf("I2C Requests: %lu total, %lu failed (%.1f%% success)\n",
                         stats.total_i2c_requests, stats.failed_i2c_requests,
                         stats.total_i2c_requests > 0 ?
                         100.0f * (stats.total_i2c_requests - stats.failed_i2c_requests) / stats.total_i2c_requests : 0.0f);
            Serial.printf("UI Updates Queued: %lu\n", stats.ui_updates_queued);
            Serial.printf("Memory Usage: %lu bytes\n", stats.memory_usage_bytes);
            Serial.println("========================");
        } else {
            Serial.println("[TASK] Tasks not active - no statistics available");
        }
    } else if (strncmp(args, "enable", 6) == 0) {
        if (task_manager::isTaskModeActive()) {
            Serial.println("[TASK] Tasks already active");
        } else {
            Serial.println("[TASK] Attempting to start FreeRTOS tasks...");
            if (task_manager::init() && task_manager::startTasks()) {
                Serial.println("[TASK] FreeRTOS tasks started successfully");
            } else {
                Serial.println("[TASK] Failed to start tasks");
            }
        }
    } else if (strncmp(args, "disable", 7) == 0) {
        if (task_manager::isTaskModeActive()) {
            Serial.println("[TASK] Stopping FreeRTOS tasks...");
            task_manager::stopTasks();
            Serial.println("[TASK] Tasks stopped - switched to legacy loop mode");
        } else {
            Serial.println("[TASK] Tasks already disabled");
        }
    } else if (strncmp(args, "control", 7) == 0) {
        args += 7;
        while (*args == ' ') args++;

        // Parse task enable/disable flags: ui i2c net sys
        // Example: task control 1 1 0 1 (enable ui, i2c, sys; disable net)
        if (strlen(args) >= 7) { // At least "1 1 1 1"
            bool ui = (args[0] == '1');
            bool i2c = (args[2] == '1');
            bool net = (args[4] == '1');
            bool sys = (args[6] == '1');

            task_manager::setTasksEnabled(ui, i2c, net, sys);
            Serial.printf("[TASK] Task control: UI=%s I2C=%s NET=%s SYS=%s\n",
                         ui ? "ON" : "OFF", i2c ? "ON" : "OFF",
                         net ? "ON" : "OFF", sys ? "ON" : "OFF");
        } else {
            Serial.println("Usage: task control <ui> <i2c> <net> <sys>");
            Serial.println("Example: task control 1 1 0 1  (enable ui,i2c,sys; disable net)");
        }
    } else {
        Serial.println("Available task commands:");
        Serial.println("  task status     - Show task status and health");
        Serial.println("  task stats      - Show task statistics");
        Serial.println("  task enable     - Start FreeRTOS tasks");
        Serial.println("  task disable    - Stop tasks, use legacy loop");
        Serial.println("  task control <ui> <i2c> <net> <sys> - Enable/disable individual tasks (1/0)");
    }
}

void handleBatteryCommand(const char* args) {
    // Skip whitespace
    while (*args == ' ') args++;

    if (strncmp(args, "status", 6) == 0 || strlen(args) == 0) {
        battery::BatteryStatus status = battery::getStatus();
        Serial.printf("[BATTERY] Voltage: %.2fV | %s | %d%%%s\n",
                      status.voltage,
                      battery::batteryStateToString(status.battery_state),
                      status.percent,
                      status.percent_is_estimate ? " (estimate)" : "");
    }
    else if (strncmp(args, "voltage", 7) == 0) {
        float voltage = battery::getVoltage();
        Serial.printf("[BATTERY] Voltage: %.2fV\n", voltage);
    }
    else if (strncmp(args, "percent", 7) == 0) {
        int percent = battery::getPercent();
        Serial.printf("[BATTERY] Battery: %d%%\n", percent);
    }
    else if (strncmp(args, "charging", 8) == 0) {
        bool charging = battery::isCharging();
        Serial.printf("[BATTERY] Charging: %s\n", charging ? "Yes" : "No");
    }
    else if (strncmp(args, "state", 5) == 0) {
        battery::BatteryState state = battery::getBatteryState();
        Serial.printf("[BATTERY] Battery State: %s\n", battery::batteryStateToString(state));
    }
    else if (strncmp(args, "raw", 3) == 0) {
        uint16_t raw = battery::getRawADC();
        Serial.printf("[BATTERY] Raw ADC: %d / 4095\n", raw);
    }
    else if (strncmp(args, "info", 4) == 0) {
        battery::printHardwareInfo();
    }
    else if (strncmp(args, "monitor", 7) == 0) {
        args += 7;
        while (*args == ' ') args++;

        if (strncmp(args, "on", 2) == 0) {
            battery::setPeriodicMonitoring(true);
        } else if (strncmp(args, "off", 3) == 0) {
            battery::setPeriodicMonitoring(false);
        } else {
            Serial.println("Usage: battery monitor on|off");
        }
    }
    else {
        Serial.println("Available battery commands:");
        Serial.println("  battery status       - Show complete battery status (voltage, state, percentage)");
        Serial.println("  battery voltage      - Show battery voltage");
        Serial.println("  battery percent      - Show battery percentage");
        Serial.println("  battery charging     - Check if battery is charging");
        Serial.println("  battery state        - Show battery state (charging/discharging/full/stable)");
        Serial.println("  battery raw          - Show raw ADC value");
        Serial.println("  battery info         - Show hardware configuration");
        Serial.println("  battery monitor on|off - Enable/disable periodic logging");
    }
}

void handleGPXCommand(const char* args) {
    // Skip whitespace
    while (*args == ' ') args++;

    if (strncmp(args, "status", 6) == 0 || strlen(args) == 0) {
        Serial.println("==== GPX Server Status ====");

        if (gpx_server::isRunning()) {
            char ip[16];
            if (gpx_server::getStatus(ip, sizeof(ip))) {
                Serial.println("Server Status:   RUNNING");
                Serial.printf("IP Address:      %s\n", ip);
                Serial.printf("Port:            80\n");
                Serial.printf("URL:             http://%s\n", ip);
                Serial.printf("WiFi Mode:       %s\n",
                    gpx_server::isRunning() ? "Access Point" : "Station");

                if (gpx_server::isRunning()) {
                    wifi_sta_list_t sta_list = {};
                    esp_wifi_ap_get_sta_list(&sta_list);
                    Serial.printf("AP SSID:         Radar-GPX\n");
                    Serial.printf("AP Password:     radar123\n");
                    Serial.printf("Connected:       %d clients\n", sta_list.num);
                }
            }
        } else {
            Serial.println("Server Status:   STOPPED");
            Serial.println("\nTo start:");
            Serial.println("  1. Connect WiFi via settings (Station mode)");
            Serial.println("  OR");
            Serial.println("  2. Use 'gpx ap' for Access Point mode");
        }

        Serial.println("===========================");
    }
    else if (strncmp(args, "ap", 2) == 0) {
        Serial.println("[GPX] Switching to Access Point mode...");
        Serial.println("[GPX] This will disconnect from current WiFi network");

        // Stop GPX server if running
        if (gpx_server::isRunning()) {
            gpx_server::stop();
        }

        // Start AP mode via gpx_server (handles AP setup internally)
        if (gpx_server::start()) {
            Serial.println("[GPX] AP and web server started successfully");
            char ip[16];
            if (gpx_server::getStatus(ip, sizeof(ip))) {
                Serial.printf("[GPX] Connect to WiFi 'Radar-GPX' and visit: http://%s\n", ip);
            }
        } else {
            Serial.println("[GPX] ERROR: Failed to start AP/web server");
        }
    }
    else if (strncmp(args, "sta", 3) == 0) {
        Serial.println("[GPX] Switching back to Station mode...");

        // Stop GPX server if running
        if (gpx_server::isRunning()) {
            gpx_server::stop();
        }

        Serial.println("[GPX] Attempting to reconnect to saved WiFi...");
        if (wifi_manager::autoConnect()) {
            Serial.println("[GPX] Auto-connect initiated");
            Serial.println("[GPX] Server will auto-start when WiFi connects");
        } else {
            Serial.println("[GPX] No saved WiFi credentials");
            Serial.println("[GPX] Use Settings screen to connect to WiFi");
        }
    }
    else if (strncmp(args, "restart", 7) == 0) {
        Serial.println("[GPX] Restarting web server...");

        if (gpx_server::isRunning()) {
            gpx_server::stop();
            delay(500);
        }

        if (gpx_server::start()) {
            char ip[16];
            if (gpx_server::getStatus(ip, sizeof(ip))) {
                Serial.printf("[GPX] Server restarted at: http://%s\n", ip);
            }
        } else {
            Serial.println("[GPX] ERROR: Failed to restart server");
        }
    }
    else {
        Serial.println("Available GPX commands:");
        Serial.println("  gpx status    - Show GPX server status");
        Serial.println("  gpx ap        - Switch to Access Point mode (for testing)");
        Serial.println("  gpx sta       - Switch back to Station mode");
        Serial.println("  gpx restart   - Restart web server");
    }
}

void handleCrashCommand(const char* args) {
    // Skip whitespace
    while (*args == ' ') args++;

    if (strncmp(args, "dump", 4) == 0 || strlen(args) == 0) {
        Serial.println("==== ESP32 Crash Dump ====");

#ifdef CONFIG_ESP_COREDUMP_ENABLE
        // Check if core dump is available in flash
        esp_core_dump_summary_t summary;
        esp_err_t err = esp_core_dump_get_summary(&summary);

        if (err == ESP_OK) {
            Serial.println("Crash dump found!");
            Serial.println("");

            // Print program counter (crash location)
            Serial.printf("Program Counter (PC): 0x%08lX\n", (unsigned long)summary.exc_pc);

            // Print task information
            char task_name[16];
            memcpy(task_name, summary.exc_task, sizeof(summary.exc_task));
            task_name[sizeof(summary.exc_task)] = '\0';
            Serial.printf("Crashed Task: %s\n", task_name);
            Serial.println("");

            Serial.printf("Core Dump Version: %d\n", summary.core_dump_version);
            Serial.printf("App ELF SHA256: ");
            for (int i = 0; i < 32; i++) {
                Serial.printf("%02X", summary.app_elf_sha256[i]);
            }
            Serial.println("");
            Serial.println("Use 'crash clear' to erase this dump after investigation");
        } else if (err == ESP_ERR_NOT_FOUND) {
            Serial.println("No crash dump found (normal if no panic since last clear)");
        } else {
            Serial.printf("Error reading crash dump: %d\n", err);
        }
#else
        Serial.println("Core dump not enabled (CONFIG_ESP_COREDUMP_ENABLE not set)");
#endif

        Serial.println("==========================");
    }
    else if (strncmp(args, "clear", 5) == 0) {
        Serial.println("[CRASH] Clearing crash dump from flash...");

        // Note: ESP32 doesn't provide direct API to clear core dump
        // The core dump will be overwritten on next panic
        Serial.println("[CRASH] Crash dump will be overwritten on next panic");
        Serial.println("[CRASH] To fully erase, reflash the firmware");
    }
    else if (strncmp(args, "info", 4) == 0) {
        Serial.println("==== Crash Logging System Info ====");
        Serial.println("Core Dump:       Enabled (Flash)");
        Serial.println("Partition:       256KB coredump partition");
        Serial.println("Debug Level:     CORE_DEBUG_LEVEL=3");
        Serial.println("");
        Serial.println("CAPABILITIES:");
        Serial.println("✓ Automatic panic capture");
        Serial.println("✓ Exception cause identification");
        Serial.println("✓ Program counter (crash location)");
        Serial.println("✓ Register dump preservation");
        Serial.println("✓ Survives reboot");
        Serial.println("");
        Serial.println("USAGE:");
        Serial.println("After unexpected reboot:");
        Serial.println("1. Reconnect serial monitor");
        Serial.println("2. Type 'crash dump' to see panic info");
        Serial.println("3. Note the Program Counter (PC) address");
        Serial.println("4. Check if exception repeats (pattern)");
        Serial.println("");
        Serial.println("LIMITATIONS:");
        Serial.println("- Cannot retrieve full stack trace via serial");
        Serial.println("- PC address requires firmware.elf for symbol lookup");
        Serial.println("- Core dump overwritten on next panic");
        Serial.println("===================================");
    }
    else {
        Serial.println("Available crash commands:");
        Serial.println("  crash dump       - Show last crash dump information");
        Serial.println("  crash clear      - Clear crash dump data");
        Serial.println("  crash info       - Show crash logging system info");
    }
}

void handleLogCommand(const char* args) {
    // Skip whitespace
    while (*args == ' ') args++;

    if (strncmp(args, "status", 6) == 0 || strlen(args) == 0) {
        Serial.println("\n=== System Logger Status ===");
        Serial.printf("Enabled: %s\n", system_logger::isEnabled() ? "yes" : "no");
        Serial.printf("Buffer usage: %zu / %zu bytes\n",
                      system_logger::getBufferUsage(), system_logger::BUFFER_SIZE);
        Serial.printf("File size: %zu bytes\n", system_logger::getFileSize());
        Serial.printf("Log file: %s\n", system_logger::LOG_FILE);
        Serial.println("==========================\n");

    } else if (strncmp(args, "flush", 5) == 0) {
        if (system_logger::flush()) {
            Serial.println("[LOG] Flush successful");
        } else {
            Serial.println("[LOG] Flush failed");
        }

    } else if (strncmp(args, "enable", 6) == 0) {
        system_logger::setEnabled(true);
        Serial.println("[LOG] Logging enabled");

    } else if (strncmp(args, "disable", 7) == 0) {
        system_logger::setEnabled(false);
        Serial.println("[LOG] Logging disabled");

    } else {
        Serial.println("Available log commands:");
        Serial.println("  log [status]     - Show logger status");
        Serial.println("  log flush        - Force flush buffer to file");
        Serial.println("  log enable       - Enable logging");
        Serial.println("  log disable      - Disable logging");
    }
}

void handleNTPCommand(const char* args) {
    // Skip whitespace
    while (*args == ' ') args++;

    if (strncmp(args, "status", 6) == 0) {
        // Show NTP sync status
        ntp_sync::printStatus();

    } else if (strncmp(args, "sync", 4) == 0) {
        Serial.println("[NTP] NTP removed — time comes from GPS only");

    } else if (strncmp(args, "settime", 7) == 0) {
        // Manual RTC time setting: ntp settime YYYY-MM-DD HH:MM:SS
        // Example: ntp settime 2025-10-24 13:40:00
        args += 7;
        while (*args == ' ') args++;

        int year, month, day, hour, minute, second;
        if (sscanf(args, "%d-%d-%d %d:%d:%d", &year, &month, &day, &hour, &minute, &second) == 6) {
            // Validate input
            if (year >= 2025 && year <= 2030 && month >= 1 && month <= 12 &&
                day >= 1 && day <= 31 && hour >= 0 && hour <= 23 &&
                minute >= 0 && minute <= 59 && second >= 0 && second <= 59) {

                // Create RTC time structure
                rtc::Time rtc_time;
                rtc_time.year = year;
                rtc_time.month = month;
                rtc_time.day = day;
                rtc_time.hour = hour;
                rtc_time.minute = minute;
                rtc_time.second = second;
                rtc_time.wday = 0;  // Will be calculated by RTC
                rtc_time.valid = true;

                // Set RTC
                if (rtc::set(rtc_time)) {
                    Serial.printf("[RTC] ✓ Time set: %04d-%02d-%02d %02d:%02d:%02d\n",
                                  year, month, day, hour, minute, second);
                    system_logger::info("RTC", "Manual time set successful");
                } else {
                    Serial.println("[RTC] ✗ Failed to set time");
                    system_logger::error("RTC", "Manual time set failed");
                }
            } else {
                Serial.println("[RTC] Invalid time parameters");
                Serial.println("Usage: ntp settime YYYY-MM-DD HH:MM:SS");
                Serial.println("Example: ntp settime 2025-10-24 13:40:00");
            }
        } else {
            Serial.println("Usage: ntp settime YYYY-MM-DD HH:MM:SS");
            Serial.println("Example: ntp settime 2025-10-24 13:40:00");
        }

    } else if (strncmp(args, "timezone", 8) == 0) {
        // Parse timezone parameters
        args += 8;
        while (*args == ' ') args++;

        // Parse GMT offset
        int gmt_offset = atoi(args);

        // Parse DST offset (optional)
        while (*args && *args != ' ') args++;
        while (*args == ' ') args++;
        int dst_offset = 0;
        if (*args) {
            dst_offset = atoi(args);
        }

        ntp_sync::setTimezone(gmt_offset, dst_offset);
        Serial.printf("[NTP] Timezone stored (GMT%+d, DST: %dh) — no NTP sync\n", gmt_offset, dst_offset);

    } else {
        Serial.println("Available NTP commands:");
        Serial.println("  ntp status           - Show NTP sync status");
        Serial.println("  ntp sync             - Force NTP time sync");
        Serial.println("  ntp settime YYYY-MM-DD HH:MM:SS - Manually set RTC time");
        Serial.println("  ntp timezone <gmt> [dst] - Set timezone (e.g., ntp timezone -5 1)");
    }
}

void handleDevCommand(const char* args) {
    // Skip whitespace
    while (*args == ' ') args++;

    if (strncmp(args, "on", 2) == 0) {
        // Enable dev mode (persists across reboots)
        settings_manager::saveDevMode(true);
        settings_manager::saveDevTabVisible(true);
        settings_manager::saveLoggingEnabled(true);
        system_logger::setEnabled(true);
        { task_manager::UIUpdate u{}; u.type = task_manager::UIUpdateType::DEV_MODE_CHANGE; task_manager::queueUIUpdate(u); }
        Serial.println("[DEV] Dev mode ON (battery voltage, logging, DEV tab)");

    } else if (strncmp(args, "off", 3) == 0) {
        // Disable dev mode (persists across reboots)
        settings_manager::saveDevMode(false);
        settings_manager::saveDevTabVisible(false);
        settings_manager::saveLoggingEnabled(false);
        system_logger::setEnabled(false);
        { task_manager::UIUpdate u{}; u.type = task_manager::UIUpdateType::DEV_MODE_CHANGE; task_manager::queueUIUpdate(u); }
        Serial.println("[DEV] Dev mode OFF (production UI, logging disabled)");

    } else if (strncmp(args, "show", 4) == 0) {
        // Legacy: enable DEV tab visibility
        settings_manager::saveDevTabVisible(true);
        Serial.println("[DEV] DEV tab is now VISIBLE in settings");

    } else if (strncmp(args, "hide", 4) == 0) {
        // Legacy: disable DEV tab visibility
        settings_manager::saveDevTabVisible(false);
        Serial.println("[DEV] DEV tab is now HIDDEN in settings");

    } else if (strncmp(args, "status", 6) == 0 || strlen(args) == 0) {
        // Show current status from cached settings
        const auto& settings = settings_manager::getSettings();
        Serial.printf("[DEV] Dev mode: %s\n", settings.dev_mode ? "ON" : "OFF");
        Serial.printf("[DEV] DEV tab: %s\n", settings.dev_tab_visible ? "VISIBLE" : "HIDDEN");
        Serial.printf("[DEV] Logging: %s\n", settings.logging_enabled ? "ENABLED" : "DISABLED");

    } else {
        Serial.println("Available dev commands:");
        Serial.println("  dev on|off           - Enable/disable dev mode (persists)");
        Serial.println("  dev show|hide        - Show/hide DEV tab in settings");
        Serial.println("  dev status           - Show current dev mode status");
    }
}

// ============================================================================
// Beacon Proximity Commands
// ============================================================================

void handleBeaconCommand(const char* args) {
    // Skip whitespace
    while (*args == ' ') args++;

    if (strncmp(args, "status", 6) == 0 || strlen(args) == 0) {
        // Show beacon proximity status
        const auto& settings = settings_manager::getSettings();
        beacon_proximity::BeaconState state = beacon_proximity::getState();

        Serial.println("=== BEACON PROXIMITY STATUS ===");
        Serial.printf("Feature enabled: %s\n", settings.beacon_proximity_enabled ? "YES" : "NO");
        Serial.printf("Target MAC: %s\n", settings.beacon_count > 0 ? settings.beacon_macs[0] : "(not set)");
        Serial.printf("Measured power: %d dBm\n", settings.beacon_measured_power);
        Serial.printf("Path loss (n): %.1f\n", settings.beacon_path_loss_n);
        Serial.println("---");
        Serial.printf("Scanning active: %s\n", state.scanning_enabled ? "YES" : "NO");
        Serial.printf("Beacon found: %s\n", state.found ? "YES" : "NO");
        Serial.printf("RSSI: %d dBm (raw), %.1f dBm (EMA)\n", state.rssi_raw, state.rssi_ema);
        Serial.printf("Zone: %s\n", beacon_proximity::zoneToString(state.zone));
        Serial.printf("Trend: %s\n", beacon_proximity::trendToString(state.trend));
        Serial.printf("Distance: %.1f m (legacy)\n", state.distance_m);
        Serial.printf("Last seen: %lu ms ago\n", state.last_seen_ms > 0 ? millis() - state.last_seen_ms : 0);
        Serial.println("===============================");

    } else if (strncmp(args, "enable", 6) == 0) {
        args += 6;
        while (*args == ' ') args++;
        if (strncmp(args, "on", 2) == 0) {
            settings_manager::saveBeaconProximityEnabled(true);
            Serial.println("[BEACON] Proximity feature ENABLED (will activate at 50m zoom)");
        } else if (strncmp(args, "off", 3) == 0) {
            settings_manager::saveBeaconProximityEnabled(false);
            beacon_proximity::setEnabled(false);
            Serial.println("[BEACON] Proximity feature DISABLED");
        } else {
            Serial.println("Usage: beacon enable on|off");
        }

    } else if (strncmp(args, "mac", 3) == 0) {
        args += 3;
        while (*args == ' ') args++;
        if (strlen(args) >= 17) {  // MAC address format: XX:XX:XX:XX:XX:XX
            settings_manager::saveBeaconMAC(args);
        } else if (strlen(args) == 0) {
            const auto& settings = settings_manager::getSettings();
            Serial.printf("[BEACON] Current MAC: %s\n", settings.beacon_count > 0 ? settings.beacon_macs[0] : "(not set)");
        } else {
            Serial.println("Usage: beacon mac XX:XX:XX:XX:XX:XX");
        }

    } else if (strncmp(args, "power", 5) == 0) {
        args += 5;
        while (*args == ' ') args++;
        if (strlen(args) > 0) {
            int power = atoi(args);
            if (power < -100 && power > 0) {
                // Likely negative value entered without minus
                power = -power;
            }
            if (power >= -100 && power <= -20) {
                settings_manager::saveBeaconMeasuredPower((int8_t)power);
            } else {
                Serial.println("Usage: beacon power -XX (typical range: -70 to -40)");
            }
        } else {
            const auto& settings = settings_manager::getSettings();
            Serial.printf("[BEACON] Current measured power: %d dBm\n", settings.beacon_measured_power);
        }

    } else if (strncmp(args, "pathloss", 8) == 0 || strncmp(args, "n", 1) == 0) {
        if (args[0] == 'n') args += 1; else args += 8;
        while (*args == ' ') args++;
        if (strlen(args) > 0) {
            float n = atof(args);
            if (n >= 2.0f && n <= 4.0f) {
                settings_manager::saveBeaconPathLoss(n);
            } else {
                Serial.println("Usage: beacon n X.X (range: 2.0-4.0, typical: 2.5 indoor)");
            }
        } else {
            const auto& settings = settings_manager::getSettings();
            Serial.printf("[BEACON] Current path loss exponent: %.1f\n", settings.beacon_path_loss_n);
        }

    } else if (strncmp(args, "test", 4) == 0) {
        // Force a scan and report results
        Serial.println("[BEACON] Forcing beacon scan...");
        beacon_proximity::setEnabled(true);
        delay(3000);  // Wait for scan
        beacon_proximity::BeaconState state = beacon_proximity::getState();
        Serial.printf("[BEACON] Result: found=%s rssi=%d dBm (raw), %.1f dBm (EMA), zone=%s, distance=%.1fm\n",
                      state.found ? "YES" : "NO", state.rssi_raw, state.rssi_ema,
                      beacon_proximity::zoneToString(state.zone), state.distance_m);
        beacon_proximity::setEnabled(false);

    } else if (strncmp(args, "scan", 4) == 0) {
        // Debug: Scan and list ALL BLE devices
        beacon_proximity::debugScanAll();

    } else if (strncmp(args, "debug", 5) == 0) {
        // Debug: Print internal state
        beacon_proximity::debugPrintState();

    } else if (strncmp(args, "zone", 4) == 0) {
        // Show current zone and hysteresis state
        beacon_proximity::BeaconState state = beacon_proximity::getState();
        Serial.println("=== BEACON ZONE STATUS ===");
        Serial.printf("Current Zone: %s\n", beacon_proximity::zoneToString(state.zone));
        Serial.printf("Pending Zone: %s (count=%d/3)\n",
                      beacon_proximity::zoneToString(state.pending_zone),
                      state.pending_zone_count);
        Serial.printf("EMA RSSI: %.1f dBm\n", state.rssi_ema);
        Serial.printf("Raw RSSI: %d dBm\n", state.rssi_raw);
        Serial.println("Thresholds: CLOSE >= -65, FAR >= -85 (±3dB hysteresis)");
        Serial.println("==========================");

    } else if (strncmp(args, "trend", 5) == 0) {
        // Show trend history and calculated slope
        beacon_proximity::BeaconState state = beacon_proximity::getState();
        Serial.println("=== BEACON TREND STATUS ===");
        Serial.printf("Current Trend: %s\n", beacon_proximity::trendToString(state.trend));
        Serial.printf("EMA RSSI: %.1f dBm\n", state.rssi_ema);
        Serial.println("Thresholds:");
        Serial.println("  APPROACHING: slope > +2 dBm/cycle");
        Serial.println("  DEPARTING:   slope < -2 dBm/cycle");
        Serial.println("  STABLE:      |slope| <= 2 dBm/cycle");
        Serial.println("===========================");

    } else if (strncmp(args, "reset", 5) == 0) {
        // Reset all smoothing/trend state
        Serial.println("[BEACON] Resetting all state...");
        beacon_proximity::resetState();
        Serial.println("[BEACON] State reset complete");

    } else {
        Serial.println("Available beacon commands:");
        Serial.println("  beacon [status]           - Show beacon proximity status");
        Serial.println("  beacon enable on|off      - Enable/disable feature");
        Serial.println("  beacon mac XX:XX:XX:XX:XX:XX - Set target beacon MAC");
        Serial.println("  beacon power -XX          - Set measured power (dBm at 1m)");
        Serial.println("  beacon n X.X              - Set path loss exponent (2.0-4.0)");
        Serial.println("  beacon test               - Force scan and report results");
        Serial.println("  beacon scan               - List ALL visible BLE devices");
        Serial.println("  beacon debug              - Print internal module state");
        Serial.println("");
        Serial.println("v2 Zone/Trend Commands:");
        Serial.println("  beacon zone               - Show current zone and hysteresis state");
        Serial.println("  beacon trend              - Show trend history and calculated slope");
        Serial.println("  beacon reset              - Reset all smoothing/trend state");
    }
}

// ============================================================================
// Compass (QMC5883L) Commands
// ============================================================================

void handleCompassCommand(const char* args) {
    // Skip whitespace
    while (*args == ' ') args++;

    // QMC5883L register addresses
    constexpr uint8_t REG_DATA     = 0x00;  // X_LSB, X_MSB, Y_LSB, Y_MSB, Z_LSB, Z_MSB
    constexpr uint8_t REG_STATUS   = 0x06;
    constexpr uint8_t REG_CONTROL1 = 0x09;
    constexpr uint8_t REG_CONTROL2 = 0x0A;
    constexpr uint8_t REG_SETRESET = 0x0B;
    constexpr uint8_t REG_CHIP_ID  = 0x0D;

    if (strncmp(args, "status", 6) == 0 || strlen(args) == 0) {
        Serial.println("==== Compass Status (QMC5883L) ====");

        // Check if device is on the bus
        bool present = i2c_manager::ping(i2c_manager::COMPASS_DEVICE);
        Serial.printf("Device (0x0D):   %s\n", present ? "DETECTED" : "NOT FOUND");

        if (!present) {
            Serial.println("Check BH-880 I2C wiring (SDA=15, SCL=7)");
            Serial.println("====================================");
            return;
        }

        // Read chip ID
        uint8_t chip_id = 0;
        if (i2c_manager::readByte(i2c_manager::COMPASS_DEVICE, REG_CHIP_ID, chip_id)) {
            Serial.printf("Chip ID:         0x%02X %s\n", chip_id,
                          chip_id == 0xFF ? "(QMC5883L confirmed)" : "(unexpected!)");
        } else {
            Serial.println("Chip ID:         READ FAILED");
        }

        // Read control register
        uint8_t ctrl1 = 0;
        if (i2c_manager::readByte(i2c_manager::COMPASS_DEVICE, REG_CONTROL1, ctrl1)) {
            const char* mode_str = (ctrl1 & 0x01) ? "Continuous" : "Standby";
            int odr_bits = (ctrl1 >> 2) & 0x03;
            const char* odr_str[] = {"10Hz", "50Hz", "100Hz", "200Hz"};
            int rng_bits = (ctrl1 >> 4) & 0x03;
            const char* rng_str[] = {"2G", "8G", "?", "?"};
            int osr_bits = (ctrl1 >> 6) & 0x03;
            const char* osr_str[] = {"512", "256", "128", "64"};

            Serial.printf("Mode:            %s\n", mode_str);
            Serial.printf("Output Rate:     %s\n", odr_str[odr_bits]);
            Serial.printf("Range:           %s\n", rng_str[rng_bits]);
            Serial.printf("Oversampling:    %s\n", osr_str[osr_bits]);
        }

        // Read status register
        uint8_t status = 0;
        if (i2c_manager::readByte(i2c_manager::COMPASS_DEVICE, REG_STATUS, status)) {
            Serial.printf("Data Ready:      %s\n", (status & 0x01) ? "YES" : "NO");
            Serial.printf("Overflow:        %s\n", (status & 0x02) ? "YES" : "NO");
            Serial.printf("Data Overrun:    %s\n", (status & 0x04) ? "YES" : "NO");
        }

        Serial.println("====================================");
    }
    else if (strncmp(args, "init", 4) == 0) {
        Serial.println("[COMPASS] Initializing QMC5883L...");

        // Step 1: Write SET/RESET period
        if (!i2c_manager::writeByte(i2c_manager::COMPASS_DEVICE, REG_SETRESET, 0x01)) {
            Serial.println("[COMPASS] Failed to write SET/RESET register");
            return;
        }

        // Step 2: Configure - Continuous mode, 200Hz ODR, 2G range, 512 OSR
        // 0x0D = 00 00 11 01 = OSR512 | RNG2G | ODR200Hz | Continuous
        uint8_t config = 0x01   // Continuous mode
                       | 0x0C;  // 200Hz ODR
        // OSR 512 = 0x00 (bits 7-6), RNG 2G = 0x00 (bits 5-4) - both zero, no OR needed

        if (!i2c_manager::writeByte(i2c_manager::COMPASS_DEVICE, REG_CONTROL1, config)) {
            Serial.println("[COMPASS] Failed to write CONTROL1 register");
            return;
        }

        // Step 3: Enable pointer rollover
        if (!i2c_manager::writeByte(i2c_manager::COMPASS_DEVICE, REG_CONTROL2, 0x40)) {
            Serial.println("[COMPASS] Failed to write CONTROL2 register");
            return;
        }

        delay(10);  // Let first measurement complete

        // Verify by reading chip ID
        uint8_t chip_id = 0;
        i2c_manager::readByte(i2c_manager::COMPASS_DEVICE, REG_CHIP_ID, chip_id);
        Serial.printf("[COMPASS] Initialized! Chip ID: 0x%02X, Config: 0x%02X\n", chip_id, config);
        Serial.println("[COMPASS] Mode: Continuous, 200Hz, 2G range, 512x oversampling");
    }
    else if (strncmp(args, "read", 4) == 0) {
        // Read raw magnetic field data
        uint8_t data[6] = {0};
        if (!i2c_manager::read(i2c_manager::COMPASS_DEVICE, REG_DATA, data, 6)) {
            Serial.println("[COMPASS] Failed to read data registers");
            Serial.println("[COMPASS] Try 'compass init' first to start measurements");
            return;
        }

        int16_t x = (int16_t)(data[1] << 8 | data[0]);
        int16_t y = (int16_t)(data[3] << 8 | data[2]);
        int16_t z = (int16_t)(data[5] << 8 | data[4]);

        float heading = atan2((float)y, (float)x) * 180.0f / M_PI;
        if (heading < 0) heading += 360.0f;

        Serial.println("==== Compass Reading ====");
        Serial.printf("Raw X: %6d\n", x);
        Serial.printf("Raw Y: %6d\n", y);
        Serial.printf("Raw Z: %6d\n", z);
        Serial.printf("Heading: %.1f° (magnetic, uncalibrated)\n", heading);
        Serial.println("=========================");
        Serial.println("Note: This is raw magnetic heading without calibration.");
        Serial.println("Rotate device to verify values change smoothly.");
    }
    else if (strncmp(args, "cal set", 7) == 0) {
        // compass cal set X Y  — manually set hard-iron offsets and persist to NVS
        const char* p = args + 7;
        while (*p == ' ') p++;
        int x_val = 0, y_val = 0;
        if (sscanf(p, "%d %d", &x_val, &y_val) == 2) {
            if (settings_manager::saveCompassCalibration((int16_t)x_val, (int16_t)y_val, 0)) {
                compass_qmc5883l::setCalibration((int16_t)x_val, (int16_t)y_val, 0);
                Serial.printf("[COMPASS] Calibration manually set: X=%d Y=%d Z=0\n", x_val, y_val);
                Serial.println("[COMPASS] Saved to NVS. Run 'compass stream' to verify.");
            } else {
                Serial.println("[COMPASS] ERROR: Failed to save to NVS");
            }
        } else {
            Serial.println("Usage: compass cal set <X> <Y>");
            Serial.println("Example: compass cal set -2093 -3465");
        }
    }
    else if (strncmp(args, "cal", 3) == 0) {
        const auto& settings = settings_manager::getSettings();
        Serial.println("==== Compass Calibration ====");
        Serial.printf("Status:    %s\n", settings.compass_calibrated ? "CALIBRATED" : "NOT CALIBRATED");
        Serial.printf("X offset:  %d\n", settings.compass_cal_x);
        Serial.printf("Y offset:  %d\n", settings.compass_cal_y);
        Serial.printf("Z offset:  %d\n", settings.compass_cal_z);

        if (settings.compass_calibrated) {
            // Quality check: offsets near zero = little hard iron distortion
            int16_t mag = (int16_t)sqrtf((float)settings.compass_cal_x * settings.compass_cal_x +
                                         (float)settings.compass_cal_y * settings.compass_cal_y);
            Serial.printf("Offset magnitude: %d\n", mag);
            if (mag < 500)        Serial.println("Quality: EXCELLENT (minimal hard iron)");
            else if (mag < 2000)  Serial.println("Quality: GOOD");
            else if (mag < 5000)  Serial.println("Quality: FAIR (significant hard iron - normal for enclosure)");
            else                  Serial.println("Quality: POOR - consider recalibrating in open area");
        }
        Serial.println("Tip: run 'compass stream' after calibration to verify 0-360 heading range");
        Serial.println("=============================");
    }
    else if (strncmp(args, "continuous", 10) == 0 || strncmp(args, "stream", 6) == 0) {
        args += (args[0] == 'c') ? 10 : 6;
        while (*args == ' ') args++;
        uint32_t duration_s = 5;
        if (*args) duration_s = atoi(args);
        if (duration_s < 1) duration_s = 5;
        if (duration_s > 30) duration_s = 30;

        const auto& cal = settings_manager::getSettings();
        Serial.printf("[COMPASS] Streaming for %lu seconds (press any key to stop)...\n", duration_s);
        Serial.printf("[COMPASS] Calibration: %s (X=%d Y=%d)\n",
                      cal.compass_calibrated ? "APPLIED" : "NONE",
                      cal.compass_cal_x, cal.compass_cal_y);
        Serial.println("  X      Y      Z    Raw Hdg  Cal Hdg");
        Serial.println("------  ------  ------  -------  -------");

        uint32_t start = millis();
        uint32_t samples = 0;

        while (millis() - start < duration_s * 1000) {
            if (Serial.available()) {
                Serial.read();
                break;
            }

            uint8_t data[6] = {0};
            if (i2c_manager::read(i2c_manager::COMPASS_DEVICE, REG_DATA, data, 6)) {
                int16_t x = (int16_t)(data[1] << 8 | data[0]);
                int16_t y = (int16_t)(data[3] << 8 | data[2]);
                int16_t z = (int16_t)(data[5] << 8 | data[4]);

                // Raw heading (no calibration)
                float raw_heading = atan2f((float)y, (float)x) * 180.0f / M_PI;
                if (raw_heading < 0) raw_heading += 360.0f;

                // Calibrated heading (apply hard-iron offsets only, no mounting compensation needed)
                float cx = x - cal.compass_cal_x;
                float cy = y - cal.compass_cal_y;
                float cal_heading = atan2f(cy, cx) * 180.0f / M_PI;
                if (cal_heading < 0) cal_heading += 360.0f;

                Serial.printf("%6d  %6d  %6d  %5.1f°   %5.1f°\n", x, y, z, raw_heading, cal_heading);
                samples++;
            }

            delay(100);  // 10 Hz display rate
        }

        Serial.printf("[COMPASS] Done: %lu samples in %lu ms\n", samples, millis() - start);
    }
    else {
        Serial.println("Available compass commands (QMC5883L on BH-880):");
        Serial.println("  compass [status]     - Show chip ID and configuration");
        Serial.println("  compass init         - Initialize sensor (continuous mode, 200Hz)");
        Serial.println("  compass read         - Read raw X/Y/Z and compute heading");
        Serial.println("  compass stream [s]   - Stream readings for N seconds (default 5)");
        Serial.println("  compass cal          - Show saved calibration offsets and quality");
        Serial.println("  compass cal set X Y  - Manually set hard-iron offsets and save to NVS");
    }
}

} // namespace diagnostics