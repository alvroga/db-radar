#include "core/arduino_compat.h"
#include <stdarg.h>
#include <string.h>
#include <stdio.h>

// ============================================================================
// Serial — backed by printf() → UART0 (default ESP-IDF console)
// Input reading uses stdin (available when UART console is connected).
// ============================================================================

// Simple ring buffer for stdin reads
#define SERIAL_RING_SIZE 256
static uint8_t  s_ring_buf[SERIAL_RING_SIZE];
static volatile size_t s_head = 0;
static volatile size_t s_tail = 0;
static inline size_t ring_next(size_t pos) { return (pos + 1) % SERIAL_RING_SIZE; }

SerialClass Serial;  // Singleton definition

void SerialClass::begin(uint32_t baud) {
    (void)baud;
    // UART0 console is initialized by ESP-IDF boot loader — nothing to do here.
}

int SerialClass::printf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int n = vprintf(fmt, args);
    va_end(args);
    fflush(stdout);
    return n;
}

void SerialClass::print(const char* s) {
    if (s) { fputs(s, stdout); fflush(stdout); }
}
void SerialClass::print(const String& s) { print(s.c_str()); }

void SerialClass::print(int v, int base) {
    char buf[24];
    if (base == 16) snprintf(buf, sizeof(buf), "%x", v);
    else            snprintf(buf, sizeof(buf), "%d", v);
    print(buf);
}
void SerialClass::print(unsigned int v, int base) {
    char buf[24];
    if (base == 16) snprintf(buf, sizeof(buf), "%x", v);
    else            snprintf(buf, sizeof(buf), "%u", v);
    print(buf);
}
void SerialClass::print(long v, int base) {
    char buf[24];
    if (base == 16) snprintf(buf, sizeof(buf), "%lx", v);
    else            snprintf(buf, sizeof(buf), "%ld", v);
    print(buf);
}
void SerialClass::print(unsigned long v, int base) {
    char buf[24];
    if (base == 16) snprintf(buf, sizeof(buf), "%lx", v);
    else            snprintf(buf, sizeof(buf), "%lu", v);
    print(buf);
}
void SerialClass::print(float v, int dec) {
    char buf[32]; snprintf(buf, sizeof(buf), "%.*f", dec, (double)v); print(buf);
}
void SerialClass::print(double v, int dec) {
    char buf[32]; snprintf(buf, sizeof(buf), "%.*f", dec, v); print(buf);
}
void SerialClass::print(char c) {
    fputc(c, stdout); fflush(stdout);
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
