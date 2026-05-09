#ifndef DIAGNOSTICS_H
#define DIAGNOSTICS_H

#include "core/arduino_compat.h"

namespace diagnostics {

// Diagnostic configuration
struct Config {
    bool wifi_enabled = true;
    bool ble_enabled = true;
    bool lvgl_freeze_enabled = false;
    uint32_t freeze_duration_ms = 1000;
};

// Diagnostic state
struct DiagState {
    // Runtime control flags
    volatile bool lvgl_freeze = false;
    volatile uint32_t lvgl_freeze_until_ms = 0;

    // Command buffer
    char cmd_buffer[64];
    uint8_t cmd_length = 0;
};

// Initialize diagnostics system
bool init(const Config& config = Config{});

// Get diagnostic state
DiagState& getDiagState();

// Main diagnostic processing (call from main loop)
void processCommands();

// Individual command handlers
void handleHelpCommand();
void handleDiagCommand(const char* args);
void handleMemoryCommand(const char* args);
void handleConfigCommand(const char* args);
void handleWiFiToggle(bool enable);
void handleBLEToggle(bool enable);
void handleLVGLFreezeToggle(bool enable);

// Configuration display functions
void showCurrentConfiguration();
void showDisplayConfiguration();
void showTimingConfiguration();
void showPinConfiguration();
void handleConfigSet(const char* args);

// Task management commands
void handleTaskCommand(const char* args);

// LVGL freeze control
bool shouldSkipLVGLFlush();
void updateLVGLFreezeState();

// Utility functions
void printAvailableCommands();
void parseCommand(const char* command);

} // namespace diagnostics

#endif // DIAGNOSTICS_H