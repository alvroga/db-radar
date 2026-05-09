#include "gps_bh880.h"
#include "driver/uart.h"
#include "driver/gpio.h"

static const uart_port_t GPS_UART = UART_NUM_1;
static const int GPS_RX_BUF = 2048;
static bool s_uart_installed = false;

// ============================================================================
// UBX Protocol Definitions
// ============================================================================

#pragma pack(push, 1)
struct UBX_NAV_PVT {
  uint32_t iTOW;        // 0:  GPS time of week (ms)
  uint16_t year;        // 4:  Year UTC
  uint8_t  month;       // 6:  Month 1-12
  uint8_t  day;         // 7:  Day 1-31
  uint8_t  hour;        // 8:  Hour 0-23
  uint8_t  min;         // 9:  Minute 0-59
  uint8_t  sec;         // 10: Second 0-59
  uint8_t  valid;       // 11: Validity flags (bit0=date, bit1=time, bit2=fullyResolved)
  uint32_t tAcc;        // 12: Time accuracy estimate (ns)
  int32_t  nano;        // 16: Fraction of second (ns)
  uint8_t  fixType;     // 20: Fix type (0=none, 2=2D, 3=3D, 4=GNSS+DR, 5=time only)
  uint8_t  flags;       // 21: Fix status flags
  uint8_t  flags2;      // 22: Additional flags
  uint8_t  numSV;       // 23: Number of satellites
  int32_t  lon;         // 24: Longitude (deg * 1e-7)
  int32_t  lat;         // 28: Latitude (deg * 1e-7)
  int32_t  height;      // 32: Height above ellipsoid (mm)
  int32_t  hMSL;        // 36: Height above mean sea level (mm)
  uint32_t hAcc;        // 40: Horizontal accuracy estimate (mm)
  uint32_t vAcc;        // 44: Vertical accuracy estimate (mm)
  int32_t  velN;        // 48: NED north velocity (mm/s)
  int32_t  velE;        // 52: NED east velocity (mm/s)
  int32_t  velD;        // 56: NED down velocity (mm/s)
  int32_t  gSpeed;      // 60: Ground speed (mm/s)
  int32_t  headMot;     // 64: Heading of motion (deg * 1e-5)
  uint32_t sAcc;        // 68: Speed accuracy estimate (mm/s)
  uint32_t headAcc;     // 72: Heading accuracy estimate (deg * 1e-5)
  uint16_t pDOP;        // 76: Position DOP (* 0.01)
  uint8_t  flags3;      // 78: Additional flags
  uint8_t  reserved[5]; // 79-83: Reserved
  int32_t  headVeh;     // 84: Heading of vehicle (deg * 1e-5)
  int16_t  magDec;      // 88: Magnetic declination (deg * 1e-2)
  uint16_t magAcc;      // 90: Magnetic declination accuracy (deg * 1e-2)
};
#pragma pack(pop)
static_assert(sizeof(UBX_NAV_PVT) == 92, "UBX_NAV_PVT must be 92 bytes");

// ============================================================================
// UBX State Machine
// ============================================================================

enum class UBXState : uint8_t {
  SYNC1,    // Looking for 0xB5
  SYNC2,    // Looking for 0x62
  CLASS,    // Message class byte
  ID,       // Message ID byte
  LEN1,     // Length low byte
  LEN2,     // Length high byte
  PAYLOAD,  // Payload bytes
  CK_A,     // Checksum A
  CK_B      // Checksum B
};

// ============================================================================
// Module State
// ============================================================================

static GPSData         s_last;

// UBX parser state
static UBXState  s_state = UBXState::SYNC1;
static uint8_t   s_msg_class;
static uint8_t   s_msg_id;
static uint16_t  s_msg_len;
static uint16_t  s_payload_idx;
static uint8_t   s_payload[96]; // NAV-PVT is 92 bytes
static uint8_t   s_ck_a, s_ck_b;

// ============================================================================
// Coordinate Validation
// ============================================================================

static bool isValidCoordinate(double lat, double lon) {
  if (isnan(lat) || isnan(lon)) return false;
  if (lat < -90.0 || lat > 90.0) return false;
  if (lon < -180.0 || lon > 180.0) return false;
  if (lat == 0.0 && lon == 0.0) return false;  // Null island
  return true;
}

// ============================================================================
// NAV-PVT Parser
// ============================================================================

static void parseNavPVT(const uint8_t* payload, GPSData& g) {
  UBX_NAV_PVT pvt;
  memcpy(&pvt, payload, sizeof(pvt));

  // Fix type: 2=2D, 3=3D, 4=GNSS+dead reckoning
  g.valid = (pvt.fixType >= 2 && pvt.fixType <= 4);

  // Position (int32 * 1e-7 -> double degrees)
  double lat = pvt.lat * 1e-7;
  double lon = pvt.lon * 1e-7;

  if (g.valid && isValidCoordinate(lat, lon)) {
    g.lat = lat;
    g.lon = lon;
  }

  // Altitude (mm -> meters)
  g.alt = pvt.hMSL * 0.001;

  // Satellites and position DOP
  g.sats = pvt.numSV;
  g.hdop = (pvt.pDOP == 9999) ? NAN : pvt.pDOP * 0.01f;

  // Speed: mm/s -> knots (1 knot = 514.444 mm/s)
  g.speed = pvt.gSpeed * 0.00194384f;

  // Heading: deg * 1e-5 -> degrees
  float heading = pvt.headMot * 1e-5f;
  if (heading < 0.0f) heading += 360.0f;
  g.course = heading;

  // hasHeading: valid fix and ground speed > ~2 knots (~1029 mm/s)
  g.hasHeading = g.valid && (pvt.gSpeed > 1029);

  // Time
  bool validDate = (pvt.valid & 0x01) != 0;
  bool validTime = (pvt.valid & 0x02) != 0;

  if (validDate && validTime && pvt.year >= 2020) {
    g.year   = pvt.year;
    g.month  = pvt.month;
    g.day    = pvt.day;
    g.hour   = pvt.hour;
    g.minute = pvt.min;
    g.second = pvt.sec;
    g.hasTime = true;
  }

  g.last_update_ms = millis();
}

// ============================================================================
// Public API
// ============================================================================

namespace gps_bh880 {

// Wait for UBX-ACK-ACK or UBX-ACK-NAK for the given (cls, id).
// Drains any NAV-PVT messages that arrive before the ACK.
// Returns true = ACK received, false = NAK or timeout.
static bool waitForAck(uint8_t expected_cls, uint8_t expected_id, uint32_t timeout_ms = 500) {
  if (!s_uart_installed) return false;

  uint8_t state   = 0;
  uint8_t f_class = 0, f_id = 0;
  uint16_t f_len  = 0, f_idx = 0;
  uint8_t ck_a    = 0, ck_b  = 0;
  uint8_t payload[2] = {0};

  uint32_t start = millis();
  while (millis() - start < timeout_ms) {
    uint8_t c;
    if (uart_read_bytes(GPS_UART, &c, 1, 0) <= 0) { delay(1); continue; }

    switch (state) {
      case 0: if (c == 0xB5) state = 1; break;
      case 1: state = (c == 0x62) ? 2 : 0; break;
      case 2: f_class = c; ck_a = c; ck_b = c; state = 3; break;
      case 3: f_id = c; ck_a += c; ck_b += ck_a; state = 4; break;
      case 4: f_len = c; ck_a += c; ck_b += ck_a; state = 5; break;
      case 5:
        f_len |= ((uint16_t)c << 8);
        ck_a += c; ck_b += ck_a;
        f_idx = 0;
        state = (f_len == 0) ? 7 : 6;
        break;
      case 6:
        if (f_idx < 2) payload[f_idx] = c;
        f_idx++;
        ck_a += c; ck_b += ck_a;
        if (f_idx >= f_len) state = 7;
        break;
      case 7: state = (c == ck_a) ? 8 : 0; break;
      case 8:
        if (c == ck_b && f_class == 0x05 && f_len == 2
            && payload[0] == expected_cls && payload[1] == expected_id) {
          bool acked = (f_id == 0x01);
          Serial.printf("[GPS] %s (cls=0x%02X id=0x%02X)\n",
                        acked ? "ACK" : "NAK", expected_cls, expected_id);
          return acked;
        }
        state = 0;
        break;
    }
  }

  Serial.printf("[GPS] ACK timeout (cls=0x%02X id=0x%02X)\n", expected_cls, expected_id);
  return false;
}

static inline void uart_write_byte(uint8_t b) {
    uart_write_bytes(GPS_UART, (const char*)&b, 1);
}

bool sendUBX(uint8_t cls, uint8_t id, const uint8_t* payload, uint16_t len) {
  if (!s_uart_installed) return false;

  uint8_t ck_a = 0, ck_b = 0;

  uart_write_byte(0xB5);
  uart_write_byte(0x62);

  uart_write_byte(cls); ck_a += cls; ck_b += ck_a;
  uart_write_byte(id);  ck_a += id;  ck_b += ck_a;

  uint8_t len_lo = len & 0xFF;
  uint8_t len_hi = (len >> 8) & 0xFF;
  uart_write_byte(len_lo); ck_a += len_lo; ck_b += ck_a;
  uart_write_byte(len_hi); ck_a += len_hi; ck_b += ck_a;

  for (uint16_t i = 0; i < len; i++) {
    uart_write_byte(payload[i]);
    ck_a += payload[i]; ck_b += ck_a;
  }

  uart_write_byte(ck_a);
  uart_write_byte(ck_b);

  return true;
}

static void uart_install_at_baud(uint32_t baud, int rxPin, int txPin) {
    // Remove previous driver if installed
    if (s_uart_installed) {
        uart_driver_delete(GPS_UART);
        s_uart_installed = false;
    }
    uart_config_t cfg = {};
    cfg.baud_rate  = (int)baud;
    cfg.data_bits  = UART_DATA_8_BITS;
    cfg.parity     = UART_PARITY_DISABLE;
    cfg.stop_bits  = UART_STOP_BITS_1;
    cfg.flow_ctrl  = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_DEFAULT;
    uart_driver_install(GPS_UART, GPS_RX_BUF, 0, 0, nullptr, 0);
    uart_param_config(GPS_UART, &cfg);
    uart_set_pin(GPS_UART, txPin, rxPin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    s_uart_installed = true;
}

uint32_t detectBaud(int rxPin, int txPin) {
  static const uint32_t rates[] = {115200, 9600, 38400, 57600, 230400, 460800};

  for (auto rate : rates) {
    Serial.printf("[GPS] Trying %u baud... ", rate);

    uart_install_at_baud(rate, rxPin, txPin);
    uart_flush(GPS_UART);
    uart_flush_input(GPS_UART);

    uint32_t start = millis();
    int sync_count = 0;
    int total_bytes = 0;
    bool last_was_b5 = false;

    while (millis() - start < 1500) {
      uint8_t c;
      if (uart_read_bytes(GPS_UART, &c, 1, pdMS_TO_TICKS(1)) > 0) {
        total_bytes++;
        if (last_was_b5 && c == 0x62) {
          sync_count++;
          if (sync_count >= 3) {
            Serial.printf("FOUND! (%d UBX sync pairs detected)\n", sync_count);
            uart_driver_delete(GPS_UART);
            s_uart_installed = false;
            return rate;
          }
        }
        last_was_b5 = (c == 0xB5);
      }
    }

    Serial.printf("no UBX (%d bytes, %d sync pairs)\n", total_bytes, sync_count);
  }

  Serial.println("[GPS] Auto-detect FAILED - no baud rate produced UBX data");
  Serial.println("[GPS] Check wiring: Module TX -> GPIO44 (ESP RX)");
  return 0;
}

void begin(uint32_t baud, int rxPin, int txPin) {
  if (baud == 0) {
    baud = detectBaud(rxPin, txPin);
    if (baud == 0) {
      Serial.println("[GPS] Falling back to 115200 baud");
      baud = 115200;
    }
  }

  uart_install_at_baud(baud, rxPin, txPin);

  s_last = GPSData{};
  s_state = UBXState::SYNC1;
  Serial.printf("[GPS] UART started at %u baud (UBX binary mode)\n", baud);

  // Flush stale data before sending config commands
  delay(150);
  uart_flush_input(GPS_UART);

  // Enable NAV-PVT output at rate=1 (every navigation solution)
  uint8_t enablePVT[] = {0x01, 0x07, 0x01};
  sendUBX(0x06, 0x01, enablePVT, sizeof(enablePVT));
  waitForAck(0x06, 0x01);
}

bool read(GPSData &out) {
  bool updated = false;

  uint8_t c;
  while (s_uart_installed && uart_read_bytes(GPS_UART, &c, 1, 0) > 0) {

    switch (s_state) {
      case UBXState::SYNC1:
        if (c == 0xB5) s_state = UBXState::SYNC2;
        break;

      case UBXState::SYNC2:
        s_state = (c == 0x62) ? UBXState::CLASS : UBXState::SYNC1;
        break;

      case UBXState::CLASS:
        s_msg_class = c;
        s_ck_a = c; s_ck_b = c;
        s_state = UBXState::ID;
        break;

      case UBXState::ID:
        s_msg_id = c;
        s_ck_a += c; s_ck_b += s_ck_a;
        s_state = UBXState::LEN1;
        break;

      case UBXState::LEN1:
        s_msg_len = c;
        s_ck_a += c; s_ck_b += s_ck_a;
        s_state = UBXState::LEN2;
        break;

      case UBXState::LEN2:
        s_msg_len |= (uint16_t)c << 8;
        s_ck_a += c; s_ck_b += s_ck_a;
        s_payload_idx = 0;

        if (s_msg_len == 0) {
          s_state = UBXState::CK_A;
        } else if (s_msg_len > sizeof(s_payload)) {
          // Payload too large for our buffer - skip this message
          s_state = UBXState::SYNC1;
        } else {
          s_state = UBXState::PAYLOAD;
        }
        break;

      case UBXState::PAYLOAD:
        s_payload[s_payload_idx++] = c;
        s_ck_a += c; s_ck_b += s_ck_a;
        if (s_payload_idx >= s_msg_len) {
          s_state = UBXState::CK_A;
        }
        break;

      case UBXState::CK_A:
        s_state = (c == s_ck_a) ? UBXState::CK_B : UBXState::SYNC1;
        break;

      case UBXState::CK_B:
        if (c == s_ck_b) {
          // Valid UBX message received
          if (s_msg_class == 0x01 && s_msg_id == 0x07 && s_msg_len == 92) {
            parseNavPVT(s_payload, s_last);
            updated = true;
          }
        }
        s_state = UBXState::SYNC1;
        break;
    }
  }

  if (updated) out = s_last;
  return updated;
}

// ============================================================================
// Configuration Commands (UBX Protocol)
// ============================================================================

bool setUpdateRate(uint32_t intervalMs) {
  if (intervalMs < 25 || intervalMs > 10000) {
    Serial.printf("[GPS] Invalid update rate: %u ms (valid: 25-10000)\n", intervalMs);
    return false;
  }

  // UBX-CFG-RATE (0x06, 0x08): 6-byte payload
  // measRate(U2) + navRate(U2) + timeRef(U2)
  uint8_t payload[6];
  payload[0] = intervalMs & 0xFF;
  payload[1] = (intervalMs >> 8) & 0xFF;
  payload[2] = 1; payload[3] = 0;  // navRate = 1 (every measurement)
  payload[4] = 0; payload[5] = 0;  // timeRef = 0 (UTC)

  Serial.printf("[GPS] Setting update rate: %u ms (%.1f Hz)\n", intervalMs, 1000.0f / intervalMs);
  sendUBX(0x06, 0x08, payload, sizeof(payload));
  return waitForAck(0x06, 0x08);
}

bool setPowerMode(uint8_t mode, uint16_t periodMs, uint16_t onTimeMs) {
  // UBX-CFG-PMS (0x06, 0x86): Power Management Settings
  // mode:
  //   0x00 = Full power (default)
  //   0x01 = Interval (duty cycle: sleeps for periodMs, wakes for onTimeMs)
  //   0x02 = Aggressive with 1Hz (continuous but low-power)
  //   0x03 = Aggressive with 2Hz
  //   0x04 = Aggressive with 4Hz
  // periodMs / onTimeMs only used in interval mode (0x01)
  uint8_t payload[8];
  payload[0] = 0x00;                         // version
  payload[1] = mode;
  payload[2] = periodMs & 0xFF;              // period low byte
  payload[3] = (periodMs >> 8) & 0xFF;       // period high byte
  payload[4] = onTimeMs & 0xFF;              // onTime low byte
  payload[5] = (onTimeMs >> 8) & 0xFF;       // onTime high byte
  payload[6] = 0x00; payload[7] = 0x00;      // reserved

  const char* mode_str = "Unknown";
  if (mode == 0x00) mode_str = "Full power";
  else if (mode == 0x01) mode_str = "Interval (duty cycle)";
  else if (mode == 0x02) mode_str = "Aggressive 1Hz";
  else if (mode == 0x03) mode_str = "Aggressive 2Hz";
  else if (mode == 0x04) mode_str = "Aggressive 4Hz";

  if (mode == 0x01) {
    Serial.printf("[GPS] CFG-PMS: %s, period=%ums, onTime=%ums\n", mode_str, periodMs, onTimeMs);
  } else {
    Serial.printf("[GPS] CFG-PMS: %s\n", mode_str);
  }

  sendUBX(0x06, 0x86, payload, sizeof(payload));
  return waitForAck(0x06, 0x86);
}

bool setUpdateRateVALSET(uint32_t intervalMs, uint8_t layers) {
  if (intervalMs < 25 || intervalMs > 10000) {
    Serial.printf("[GPS] Invalid interval: %u ms (valid: 25-10000)\n", intervalMs);
    return false;
  }

  // UBX-CFG-VALSET (0x06, 0x8A)
  // Payload: version(1) + layers(1) + reserved(2) + key(4) + value(2)
  //
  // Key CFG-RATE-MEAS = 0x30210001
  //   0x3 = U2 value type (2-byte unsigned int)
  //   0x021 = RATE group
  //   0x0001 = MEAS item
  //
  // Layers: 0x01=RAM only, 0x02=BBR, 0x04=Flash, 0x07=all
  uint8_t payload[10];
  payload[0] = 0x00;                        // version
  payload[1] = layers;                      // target layer(s)
  payload[2] = 0x00; payload[3] = 0x00;    // reserved
  // key 0x30210001 little-endian
  payload[4] = 0x01; payload[5] = 0x00;
  payload[6] = 0x21; payload[7] = 0x30;
  // value: intervalMs as uint16 little-endian
  payload[8] = intervalMs & 0xFF;
  payload[9] = (intervalMs >> 8) & 0xFF;

  Serial.printf("[GPS] VALSET CFG-RATE-MEAS = %u ms (layers=0x%02X)\n", intervalMs, layers);
  sendUBX(0x06, 0x8A, payload, sizeof(payload));
  return waitForAck(0x06, 0x8A);
}

bool setBaudrate(uint32_t baud) {
  const uint32_t validBauds[] = {9600, 115200, 230400, 460800, 921600};
  bool valid = false;
  for (auto vb : validBauds) {
    if (baud == vb) { valid = true; break; }
  }

  if (!valid) {
    Serial.printf("[GPS] Invalid baudrate: %u\n", baud);
    return false;
  }

  // UBX-CFG-PRT (0x06, 0x00): 20-byte payload for UART1
  uint8_t payload[20] = {0};
  payload[0] = 1;  // portID = 1 (UART1)
  // bytes 1-3: reserved, txReady (all 0)
  // mode: 8N1 = 0x000008D0
  payload[4] = 0xD0; payload[5] = 0x08;
  payload[6] = 0x00; payload[7] = 0x00;
  // baudRate (little-endian)
  payload[8]  = baud & 0xFF;
  payload[9]  = (baud >> 8) & 0xFF;
  payload[10] = (baud >> 16) & 0xFF;
  payload[11] = (baud >> 24) & 0xFF;
  // inProtoMask: UBX+NMEA = 0x0003
  payload[12] = 0x03; payload[13] = 0x00;
  // outProtoMask: UBX = 0x0001
  payload[14] = 0x01; payload[15] = 0x00;
  // flags, reserved2: 0

  // NOTE: No ACK wait for baud change — module switches baud immediately,
  // making the ACK unreadable on the old baud rate.
  bool result = sendUBX(0x06, 0x00, payload, sizeof(payload));
  if (result) {
    Serial.printf("[GPS] Baudrate change to %u sent - reconnect serial!\n", baud);
  }
  return result;
}

bool hotStart() {
  // UBX-CFG-RST (0x06, 0x04): navBbrMask=0x0000 (keep all), resetMode=0x02 (GPS only)
  uint8_t payload[] = {0x00, 0x00, 0x02, 0x00};
  Serial.println("[GPS] Hot start (UBX-CFG-RST, keep all BBR data)");
  sendUBX(0x06, 0x04, payload, sizeof(payload));
  return waitForAck(0x06, 0x04);
}

bool warmStart() {
  // UBX-CFG-RST: navBbrMask=0x0001 (clear ephemeris), resetMode=0x02
  uint8_t payload[] = {0x01, 0x00, 0x02, 0x00};
  Serial.println("[GPS] Warm start (UBX-CFG-RST, clear ephemeris)");
  sendUBX(0x06, 0x04, payload, sizeof(payload));
  return waitForAck(0x06, 0x04);
}

bool coldStart() {
  // UBX-CFG-RST: navBbrMask=0xFFFF (clear all), resetMode=0x02
  uint8_t payload[] = {0xFF, 0xFF, 0x02, 0x00};
  Serial.println("[GPS] Cold start (UBX-CFG-RST, clear all BBR data)");
  sendUBX(0x06, 0x04, payload, sizeof(payload));
  return waitForAck(0x06, 0x04);
}

bool factoryReset() {
  // UBX-CFG-CFG (0x06, 0x09): clear all, load defaults
  // clearMask=0x0000001F, saveMask=0x00000000, loadMask=0x0000001F
  uint8_t payload[12] = {0};
  // clearMask
  payload[0] = 0x1F; payload[1] = 0x00; payload[2] = 0x00; payload[3] = 0x00;
  // saveMask (all zeros - don't save)
  // loadMask
  payload[8] = 0x1F; payload[9] = 0x00; payload[10] = 0x00; payload[11] = 0x00;

  Serial.println("[GPS] Factory reset (UBX-CFG-CFG, clear+load defaults)");
  sendUBX(0x06, 0x09, payload, sizeof(payload));
  return waitForAck(0x06, 0x09);
}

bool saveConfig() {
  // UBX-CFG-CFG (0x06, 0x09): save all sections
  // clearMask=0, saveMask=0x1F, loadMask=0
  uint8_t payload[12] = {0};
  // saveMask
  payload[4] = 0x1F; payload[5] = 0x00; payload[6] = 0x00; payload[7] = 0x00;

  sendUBX(0x06, 0x09, payload, sizeof(payload));
  bool result = waitForAck(0x06, 0x09);
  if (result) {
    Serial.println("[GPS] Configuration saved to flash");
  }
  return result;
}

void printModuleInfo() {
  if (!s_uart_installed) {
    Serial.println("[GPS] No serial port initialized");
    return;
  }

  // Flush stale data
  uart_flush_input(GPS_UART);

  // Poll UBX-MON-VER (0x0A, 0x04) — no payload
  sendUBX(0x0A, 0x04, nullptr, 0);

  // Read raw bytes looking for UBX-MON-VER response (class=0x0A, id=0x04)
  // Frame: B5 62 0A 04 [len_lo] [len_hi] [payload...] [ck_a] [ck_b]
  uint8_t buf[256] = {0};
  int buf_idx = 0;
  bool in_frame = false;
  uint16_t payload_len = 0;
  uint8_t frame_class = 0, frame_id = 0;
  uint8_t ck_a = 0, ck_b = 0;
  uint8_t state = 0; // 0=sync1, 1=sync2, 2=class, 3=id, 4=len1, 5=len2, 6=payload, 7=cka, 8=ckb

  uint32_t start = millis();
  while (millis() - start < 2000) {
    uint8_t c; if (uart_read_bytes(GPS_UART, &c, 1, pdMS_TO_TICKS(1)) <= 0) continue;

    switch (state) {
      case 0: if (c == 0xB5) state = 1; break;
      case 1: state = (c == 0x62) ? 2 : 0; break;
      case 2: frame_class = c; ck_a = c; ck_b = c; state = 3; break;
      case 3: frame_id = c; ck_a += c; ck_b += ck_a; state = 4; break;
      case 4: payload_len = c; ck_a += c; ck_b += ck_a; state = 5; break;
      case 5:
        payload_len |= ((uint16_t)c << 8);
        ck_a += c; ck_b += ck_a;
        buf_idx = 0;
        state = (payload_len == 0) ? 7 : 6;
        break;
      case 6:
        if (buf_idx < (int)sizeof(buf)) buf[buf_idx] = c;
        buf_idx++;
        ck_a += c; ck_b += ck_a;
        if (buf_idx >= payload_len) state = 7;
        break;
      case 7: in_frame = (c == ck_a); state = 8; break;
      case 8:
        if (in_frame && c == ck_b && frame_class == 0x0A && frame_id == 0x04) {
          // Valid MON-VER response — parse it
          Serial.println("[GPS] ===== Module Info (UBX-MON-VER) =====");

          char sw[31] = {0};
          char hw[11] = {0};
          memcpy(sw, buf, 30);
          memcpy(hw, buf + 30, 10);
          Serial.printf("[GPS] SW Version : %s\n", sw);
          Serial.printf("[GPS] HW Version : %s\n", hw);

          // Extension strings — 30 bytes each, variable count
          int ext_count = (payload_len - 40) / 30;
          for (int i = 0; i < ext_count; i++) {
            char ext[31] = {0};
            memcpy(ext, buf + 40 + i * 30, 30);
            Serial.printf("[GPS] Extension  : %s\n", ext);
          }
          Serial.println("[GPS] =========================================");
          return;
        }
        state = 0;
        break;
    }
  }

  Serial.println("[GPS] MON-VER timeout — no valid response received");
  Serial.println("[GPS] Check wiring and baud rate");
}

bool ping(uint32_t timeout_ms) {
  if (!s_uart_installed) {
    Serial.println("[GPS] No serial port initialized");
    return false;
  }

  // Flush any stale data
  uart_flush_input(GPS_UART);

  // Send UBX-MON-VER poll (no payload) — module responds immediately with firmware version
  Serial.println("[GPS] Sending UBX-MON-VER poll...");
  sendUBX(0x0A, 0x04, nullptr, 0);

  // Wait for UBX sync pair 0xB5 0x62
  uint32_t start = millis();
  bool got_b5 = false;
  while (millis() - start < timeout_ms) {
    { uint8_t c; if (uart_read_bytes(GPS_UART, &c, 1, 0) > 0) {
      if (got_b5 && c == 0x62) {
        Serial.printf("[GPS] PONG! Response in %lu ms — TX/RX lines OK\n", millis() - start);
        return true;
      }
      got_b5 = (c == 0xB5);
    } }
    delay(1);
  }

  Serial.println("[GPS] PING FAILED — no response within timeout");
  Serial.println("  Check: Module TX -> GPIO44 (ESP RX)");
  Serial.println("         Module RX -> GPIO43 (ESP TX)");
  return false;
}

// Known UBX message names for the summary table
static const char* ubxMsgName(uint8_t cls, uint8_t id) {
  if (cls == 0x01) {
    switch (id) {
      case 0x07: return "NAV-PVT";
      case 0x03: return "NAV-STATUS";
      case 0x04: return "NAV-DOP";
      case 0x12: return "NAV-VELNED";
      case 0x21: return "NAV-TIMEUTC";
      case 0x22: return "NAV-CLOCK";
      case 0x35: return "NAV-SAT";
      case 0x43: return "NAV-SIG";
      default:   return "NAV-???";
    }
  }
  if (cls == 0x05) return (id == 0x01) ? "ACK-ACK" : "ACK-NAK";
  if (cls == 0x0A) {
    switch (id) {
      case 0x04: return "MON-VER";
      case 0x09: return "MON-HW";
      case 0x36: return "MON-COMMS";
      case 0x38: return "MON-RF";
      default:   return "MON-???";
    }
  }
  if (cls == 0x0B) return "AID-???";
  if (cls == 0x27) return "SEC-???";
  return "UNK";
}

void dumpRaw(uint32_t duration_ms) {
  if (!s_uart_installed) {
    Serial.println("[GPS] No serial port initialized");
    return;
  }

  // Message type counter table (up to 24 unique types)
  struct MsgCount { uint8_t cls; uint8_t id; uint32_t count; };
  static MsgCount table[24];
  uint8_t table_len = 0;
  memset(table, 0, sizeof(table));

  // UBX frame parser state
  uint8_t  state = 0;
  uint8_t  f_cls = 0, f_id = 0;
  uint16_t f_len = 0, f_idx = 0;
  uint8_t  ck_a  = 0, ck_b  = 0;

  uint32_t start      = millis();
  uint32_t bytes_read = 0;
  uint32_t good_msgs  = 0;
  uint32_t bad_crc    = 0;

  Serial.printf("[GPS] Counting UBX messages for %lu ms...\n", duration_ms);

  while (millis() - start < duration_ms) {
    if (Serial.available()) { Serial.read(); break; }

    uint8_t c;
    while (uart_read_bytes(GPS_UART, &c, 1, 0) > 0) {
      bytes_read++;

      switch (state) {
        case 0: if (c == 0xB5) state = 1; break;
        case 1: state = (c == 0x62) ? 2 : 0; break;
        case 2: f_cls = c; ck_a = c; ck_b = c; state = 3; break;
        case 3: f_id  = c; ck_a += c; ck_b += ck_a; state = 4; break;
        case 4: f_len = c; ck_a += c; ck_b += ck_a; state = 5; break;
        case 5:
          f_len |= ((uint16_t)c << 8);
          ck_a += c; ck_b += ck_a;
          f_idx = 0;
          state = (f_len == 0) ? 7 : 6;
          break;
        case 6:
          f_idx++; ck_a += c; ck_b += ck_a;
          if (f_idx >= f_len) state = 7;
          break;
        case 7: state = (c == ck_a) ? 8 : 0; if (c != ck_a) bad_crc++; break;
        case 8:
          if (c == ck_b) {
            good_msgs++;
            // Find or insert into table
            bool found = false;
            for (uint8_t i = 0; i < table_len; i++) {
              if (table[i].cls == f_cls && table[i].id == f_id) {
                table[i].count++;
                found = true;
                break;
              }
            }
            if (!found && table_len < 24) {
              table[table_len++] = {f_cls, f_id, 1};
            }
          } else {
            bad_crc++;
          }
          state = 0;
          break;
      }
    }
    delay(1);
  }

  uint32_t elapsed = millis() - start;

  Serial.println("-------- UBX MESSAGE BREAKDOWN --------");
  for (uint8_t i = 0; i < table_len; i++) {
    uint32_t rate_x10 = (table[i].count * 10000UL) / elapsed; // rate * 10, no floats
    Serial.print("  ");
    Serial.print(ubxMsgName(table[i].cls, table[i].id));
    Serial.print(": ");
    Serial.print(table[i].count);
    Serial.print(" msgs (");
    Serial.print(rate_x10 / 10);
    Serial.print(".");
    Serial.print(rate_x10 % 10);
    Serial.println(" Hz)");
  }
  Serial.println("  ---");
  Serial.print("  Total: "); Serial.print(good_msgs);
  Serial.print(" msgs, "); Serial.print(bad_crc);
  Serial.print(" bad CRC, "); Serial.print(bytes_read);
  Serial.print(" bytes in "); Serial.print(elapsed);
  Serial.println(" ms");
  Serial.println("---------------------------------------");

  if (bytes_read == 0) {
    Serial.println("[GPS] No data received! Check:");
    Serial.println("  - Module power (5V on V pin)");
    Serial.println("  - TX pin connected to GPIO44 (ESP RX)");
    Serial.println("  - Baud rate match (expecting 115200)");
  }
}

} // namespace gps_bh880
