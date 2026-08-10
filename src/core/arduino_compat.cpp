#include "core/arduino_compat.h"
#include <stdarg.h>
#include <string.h>
#include <stdio.h>

// ============================================================================
// Serial — backed by printf() → the ESP-IDF console, which this project
// configures as USB CDC (CONFIG_ESP_CONSOLE_USB_CDC=y, UART_NUM=-1).
// Input reading uses stdin.
//
// Output buffering (backlog §3.5, verified against IDF 5.5 rather than assumed):
//
//   * stdout is **line buffered**. IDF reports S_IFCHR from _fstat_r_console
//     (newlib/src/reent_syscalls.c:79, with the comment saying so explicitly),
//     which makes newlib pick _IOLBF. So a trailing '\n' already flushes.
//   * The write path **cannot stall**. cdcacm_write (esp_vfs_console/
//     vfs_cdcacm.c:81) → esp_usb_console_write_buf → esp_usb_console_flush_internal
//     → cdc_acm_fifo_fill, which rolls back and returns 0 when the host is not
//     draining. It silently drops bytes; it never waits. The only
//     xSemaphoreTake(..., portMAX_DELAY) in that file is on the *read* side, and
//     s_blocking is false by default anyway.
//
// Those two facts are why the unconditional fflush(stdout) that used to end every
// call here is gone. It was not protecting against a stall — there is no stall to
// protect against — and on a line-buffered stream it was worse than redundant: it
// forced a partial write for every fragment of a composed line, so
// `print("x"); print(1); print("\r\n")` became three cdcacm_write calls, each of
// which loops the VFS layer one byte at a time. Explicit Serial.flush() remains
// for the cases that genuinely want a partial line out (e.g. before a reboot).
// ============================================================================

// Simple ring buffer for stdin reads
#define SERIAL_RING_SIZE 256
static uint8_t  s_ring_buf[SERIAL_RING_SIZE];
static volatile size_t s_head = 0;
static volatile size_t s_tail = 0;
static inline size_t ring_next(size_t pos) { return (pos + 1) % SERIAL_RING_SIZE; }

SerialClass Serial;  // Singleton definition

// Output gate. Formatting and the per-byte VFS write are the real cost of a log
// line on this console, so the gate is checked before either happens.
static bool s_log_enabled = true;

void SerialClass::setLogEnabled(bool enabled) { s_log_enabled = enabled; }
bool SerialClass::isLogEnabled() { return s_log_enabled; }

void SerialClass::begin(uint32_t baud) {
    (void)baud;
    // The console is initialized by the ESP-IDF boot loader — nothing to do here.
}

int SerialClass::printf(const char* fmt, ...) {
    if (!s_log_enabled) return 0;
    va_list args;
    va_start(args, fmt);
    int n = vprintf(fmt, args);
    va_end(args);
    return n;
}

void SerialClass::print(const char* s) {
    if (!s_log_enabled) return;
    if (s) fputs(s, stdout);
}
void SerialClass::print(const String& s) { print(s.c_str()); }

// The numeric overloads gate before snprintf, not just before the write —
// formatting is the half of the cost that print(const char*) cannot skip for them.
void SerialClass::print(int v, int base) {
    if (!s_log_enabled) return;
    char buf[24];
    if (base == 16) snprintf(buf, sizeof(buf), "%x", v);
    else            snprintf(buf, sizeof(buf), "%d", v);
    print(buf);
}
void SerialClass::print(unsigned int v, int base) {
    if (!s_log_enabled) return;
    char buf[24];
    if (base == 16) snprintf(buf, sizeof(buf), "%x", v);
    else            snprintf(buf, sizeof(buf), "%u", v);
    print(buf);
}
void SerialClass::print(long v, int base) {
    if (!s_log_enabled) return;
    char buf[24];
    if (base == 16) snprintf(buf, sizeof(buf), "%lx", v);
    else            snprintf(buf, sizeof(buf), "%ld", v);
    print(buf);
}
void SerialClass::print(unsigned long v, int base) {
    if (!s_log_enabled) return;
    char buf[24];
    if (base == 16) snprintf(buf, sizeof(buf), "%lx", v);
    else            snprintf(buf, sizeof(buf), "%lu", v);
    print(buf);
}
void SerialClass::print(float v, int dec) {
    if (!s_log_enabled) return;
    char buf[32]; snprintf(buf, sizeof(buf), "%.*f", dec, (double)v); print(buf);
}
void SerialClass::print(double v, int dec) {
    if (!s_log_enabled) return;
    char buf[32]; snprintf(buf, sizeof(buf), "%.*f", dec, v); print(buf);
}
void SerialClass::print(char c) {
    if (!s_log_enabled) return;
    fputc(c, stdout);
}

void SerialClass::println(const char* s) { print(s); print("\r\n"); }
void SerialClass::println(const String& s) { println(s.c_str()); }
void SerialClass::println(int v, int b)           { print(v, b); print("\r\n"); }
void SerialClass::println(unsigned int v, int b)  { print(v, b); print("\r\n"); }
void SerialClass::println(long v, int b)          { print(v, b); print("\r\n"); }
void SerialClass::println(unsigned long v, int b) { print(v, b); print("\r\n"); }
void SerialClass::println(float v, int d)         { print(v, d); print("\r\n"); }
void SerialClass::println(double v, int d)        { print(v, d); print("\r\n"); }

int SerialClass::available() {
    // Poll stdin for available bytes
    int c = fgetc(stdin);
    if (c == EOF) return (s_head != s_tail) ? 1 : 0;
    size_t next = ring_next(s_head);
    if (next != s_tail) {
        s_ring_buf[s_head] = (uint8_t)c;
        s_head = next;
    }
    return 1;
}

int SerialClass::read() {
    // Try to fill ring from stdin first
    available();
    if (s_head == s_tail) return -1;
    uint8_t c = s_ring_buf[s_tail];
    s_tail = ring_next(s_tail);
    return c;
}

int SerialClass::peek() {
    available();
    if (s_head == s_tail) return -1;
    return s_ring_buf[s_tail];
}

String SerialClass::readString() {
    String result;
    uint32_t start = millis();
    while (millis() - start < 100) {
        int c = read();
        if (c < 0) { vTaskDelay(1); continue; }
        if (c == '\n' || c == '\r') break;
        result += (char)c;
    }
    return result;
}

String SerialClass::readStringUntil(char terminator) {
    String result;
    uint32_t start = millis();
    while (millis() - start < 1000) {
        int c = read();
        if (c < 0) { vTaskDelay(1); continue; }
        if (c == terminator) break;
        result += (char)c;
    }
    return result;
}

void SerialClass::flush() {
    fflush(stdout);
}
