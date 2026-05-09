# WiFi Implementation Guide

**Status**: Complete ✅ | ESP-IDF native WiFi stack

This document covers the complete WiFi architecture: standalone boot modes (AP and STA), the GPX web portal, OTA firmware updates, and NVS credential storage.

---

## Table of Contents

1. [Architecture Overview](#architecture-overview)
2. [Boot Modes](#boot-modes)
3. [WiFi AP Mode (Standalone)](#wifi-ap-mode-standalone)
4. [WiFi STA Mode (Standalone)](#wifi-sta-mode-standalone)
5. [Settings-Based WiFi (In-Session)](#settings-based-wifi-in-session)
6. [GPX Web Portal](#gpx-web-portal)
7. [OTA Firmware Update](#ota-firmware-update)
8. [NVS Storage](#nvs-storage)
9. [Boot Mode Safety](#boot-mode-safety)
10. [Security Model](#security-model)
11. [Code References](#code-references)

---

## Architecture Overview

WiFi operates in three distinct modes, each with a different entry point:

| Mode | Entry | Radar Active | Web Server |
|---|---|---|---|
| **AP Standalone Boot** | Settings → reboot | No | Yes (AP SSID `Radar-GPX`) |
| **STA Standalone Boot** | Settings → reboot | No | Yes (device IP on home network) |
| **In-Session (STA)** | Settings toggle, no reboot | Yes | Yes (IP shown in WiFi tab) |

**Mutual exclusion**: AP and STA modes cannot run simultaneously. The WiFi and BLE radio stacks are also mutually exclusive — NimBLE is only initialised in radar mode (no WiFi active).

---

## Boot Modes

Two NVS flags in the `radar` namespace control boot behavior:

```
wifi_ap_en   (u8, bool) — boot into Access Point mode
wifi_sta_en  (u8, bool) — boot into WiFi STA mode and auto-connect
```

On boot, `settings_manager::loadSettings()` reads these flags. If either is set, the corresponding WiFi mode is started before the UI transitions out of the loading screen.

**Important**: After any firmware update (USB or OTA) the flags are cleared automatically — the device always boots to radar mode on first post-flash boot. See [Boot Mode Safety](#boot-mode-safety).

---

## WiFi AP Mode (Standalone)

The device creates its own WiFi access point. No router required.

**Default credentials:**
- SSID: `Radar-GPX`
- Password: `radar123`

These are hardcoded in `src/gpx/gpx_server.cpp`:
```cpp
static const char* AP_SSID     = "Radar-GPX";
static const char* AP_PASSWORD = "radar123";
```

**Enabling AP boot mode:**
1. Go to Settings → WiFi → Access Point section
2. Toggle "AP Boot Mode" on
3. Confirm reboot when prompted
4. On reboot, device creates the AP, starts web server, and shows the IP on screen

**Web portal URL:** `http://192.168.4.1` (default ESP32 AP IP)

**Hardware path:**
```
main.cpp:setup() → settings.wifi_ap_enabled == true
  → wifi_manager::init()
  → gpx_server::start()   (creates AP + HTTP server)
  → UI: LOAD_AP_SCREEN
```

---

## WiFi STA Mode (Standalone)

The device connects to a saved WiFi network on boot without showing the radar screen. Useful for firmware updates or bulk GPX management sessions.

**Enabling STA boot mode:**
1. Go to Settings → WiFi → connect to a network (credentials saved to NVS)
2. Toggle "STA Boot Mode" on
3. Confirm reboot when prompted
4. On reboot, device auto-connects and shows the web portal URL

**Web portal URL:** displayed on the STA boot screen (varies by network DHCP)

**Hardware path:**
```
main.cpp:setup() → settings.wifi_sta_boot == true
  → wifi_manager::init()
  → wifi_manager::autoConnect()   (uses saved NVS credentials)
  → UI: LOAD_WIFI_SCREEN
```

---

## Settings-Based WiFi (In-Session)

Standard WiFi — radar stays active, WiFi runs alongside it. Used for GPX uploads during a navigation session without rebooting.

**Enabling:**
1. Settings → WiFi tab → toggle WiFi on
2. Scan networks, select SSID, enter password
3. Web portal URL appears in the WiFi tab once connected

**Differences from standalone modes:**
- Radar screen stays visible
- BLE beacon scanning is disabled while WiFi is active
- WiFi auto-starts on boot only if `wifi_enabled` was left on (session-only flag, not persisted across reboots)

---

## GPX Web Portal

HTTP server (`esp_http_server`) with four endpoints:

| Endpoint | Method | Description |
|---|---|---|
| `/` | GET | GPX upload page (dark theme, drag-and-drop) |
| `/upload` | POST | Upload a `.gpx` file (`?filename=` query param, raw body) |
| `/list` | GET | JSON array of files in `/gpx/` |
| `/download/gpx/<file>` | GET | Download a specific GPX file |
| `/delete/<file>` | DELETE | Delete a GPX file |
| `/logs` | GET | System log viewer |
| `/update` | GET | OTA firmware update page |
| `/update` | POST | Flash new firmware binary |

**File storage:** FFat filesystem, `/gpx/` folder. All `.gpx` files in this folder are auto-loaded on boot.

**Implementation:** `src/gpx/gpx_server.cpp`

---

## OTA Firmware Update

Accessible at `/update`. The device writes the uploaded `.bin` directly to the inactive OTA partition.

**Partition layout** (`partitions/partitions_ota.csv`):
```
nvs       0x9000   16KB
otadata   0xE000    8KB
ota_0     0x10000   2MB   ← active or standby
ota_1     0x210000  2MB   ← active or standby
ffat      0x410000 11.7MB
coredump  0xFC0000 256KB
```

**Upload flow:**
1. Browse to `/update`
2. Select `.bin` file (built by `pio run`)
3. Click "Flash firmware" — progress bar shows upload %
4. On success, page replaces itself with a reboot notice (no retry possible)
5. Device calls `prepareForOTAReboot()`, sets boot partition, restarts

**One-shot guard:** `static bool ota_already_triggered` resets only on reboot. A stale browser tab or retry cannot re-flash the device in the same session.

**Rollback safety:** `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` is set in `sdkconfig.defaults`. New firmware must call `esp_ota_mark_app_valid_cancel_rollback()` within the boot window or the bootloader reverts to the previous partition. This call is made in `main.cpp` after the radar screen transition is queued.

---

## NVS Storage

Two namespaces are used:

| Namespace | Key | Type | Content |
|---|---|---|---|
| `radar` | `wifi_ap_en` | u8 bool | AP standalone boot enabled |
| `radar` | `wifi_sta_en` | u8 bool | STA standalone boot enabled |
| `radar` | `fw_stamp` | u32 | Per-build Unix timestamp (boot mode guard) |
| `wifi` | `ssid` | str | Saved network SSID |
| `wifi` | `password` | str | Saved network password |

The `wifi` namespace is intentionally separate from `radar` so that OTA updates and stamp resets never clear credentials.

**API:**
```cpp
settings_manager::saveWiFiAPEnabled(bool);   // radar namespace
settings_manager::saveWiFiSTABoot(bool);     // radar namespace
settings_manager::saveWiFi(ssid, password);  // wifi namespace
settings_manager::loadWiFi(ssid, password);  // wifi namespace
settings_manager::prepareForOTAReboot();     // stamp=0 + clear boot flags
```

---

## Boot Mode Safety

After any firmware flash (USB upload or OTA), the device always boots to radar mode on the first boot, regardless of previous WiFi settings.

**Mechanism:**
- `scripts/gen_version.py` writes `FW_BUILD_TS` (Unix epoch timestamp) to `include/core/fw_version_gen.h` on every build
- `FW_STAMP_VAL = FW_BUILD_TS` — unique per build
- On boot, `loadSettings()` compares stored `fw_stamp` to `FW_STAMP_VAL`. A mismatch clears `wifi_ap_en` and `wifi_sta_en` and writes the new stamp
- Any new build produces a new timestamp → stamp mismatch → radar mode

**OTA extra guarantee:**
- `prepareForOTAReboot()` writes `fw_stamp=0` to NVS before restart
- `FW_BUILD_TS` is always a real Unix timestamp (> 1 billion) — `0` never matches
- Belt-and-suspenders: even if the build timestamp somehow matches a stored value, the explicit `0` write guarantees a mismatch

---

## Security Model

**Current state:**
- The OTA endpoint (`POST /update`) has no authentication beyond network access
- In AP mode, the WPA2 password (`radar123`) is the only barrier — anyone who knows it can reach the web server
- In STA mode, anyone on the same local network can reach the web server
- WiFi is opt-in: the device boots to radar mode (no network) by default

**For open source use:**
- The default AP credentials (`Radar-GPX` / `radar123`) are public knowledge once the source is published — users should be advised to change them
- The AP SSID and password are currently hardcoded in `gpx_server.cpp` and not configurable from the settings UI — making them NVS-configurable would be the correct fix
- OTA endpoint protection (HTTP Basic Auth with a configurable password) is recommended for any deployment outside a trusted local network

**Threat model:**
- Device is not always-on — WiFi only activates when the user explicitly enables it
- OTA requires physical proximity (WiFi range) or access to the same LAN
- The primary threat is a known-default-password AP being exploited by a nearby attacker during an update session

---

## Code References

| File | Purpose |
|---|---|
| `src/gpx/gpx_server.cpp` | HTTP server, AP init, OTA handler, all web endpoints |
| `src/hardware/connectivity/wifi_manager.cpp` | STA connection state machine |
| `src/hardware/connectivity/scanner.cpp` | WiFi/BLE network scanning |
| `src/utils/settings_manager.cpp` | NVS storage, boot flags, stamp mechanism |
| `src/ui/settings_screen.cpp` | WiFi settings UI, AP/STA toggles |
| `src/ui/ui_manager.cpp` | AP boot screen, STA boot screen |
| `src/core/main.cpp` | Boot phase 5: applies WiFi flags, starts AP/STA |
| `scripts/gen_version.py` | Pre-build: writes `FW_VERSION` + `FW_BUILD_TS` |
| `partitions/partitions_ota.csv` | Dual OTA partition layout |
| `sdkconfig.defaults` | `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` |
