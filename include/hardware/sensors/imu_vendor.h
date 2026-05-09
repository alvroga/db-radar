#pragma once
#include "core/arduino_compat.h"

// Config struct so you can adjust behavior if needed
struct ImuCfg {
    float lsb_per_g = 16384.0f;   // 4096.0f if your unit uses that scale
    int   rot_deg   = 90;         // 0/90/180/270
    int   flip_x    = -1;         // +1 or -1
    int   flip_y    = -1;         // +1 or -1 (screen Y grows downward)
};

// Initialize the IMU (returns true if detected)
bool imu_begin(const ImuCfg& cfg = ImuCfg{});

// Read smoothed acceleration in g-units
// Returns false if read failed (will keep last values)
bool imu_read(float& ax, float& ay, float& az);

// Map accel into screen coordinates (px,py) centered at (cx,cy)
// radius = max travel in pixels
bool imu_read_screen(int& px, int& py, int cx, int cy, int radius);