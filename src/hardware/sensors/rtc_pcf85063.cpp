#include "rtc_pcf85063.h"
#include "i2c_manager.h"

namespace rtc {

static uint8_t g_addr = 0x51;

static const uint8_t REG_RAM     = 0x03;
static const uint8_t REG_SECONDS = 0x04;  // +6 bytes: MIN,HOUR,DAY,WKDAY,MONTH,YEAR
static const uint8_t INIT_MAGIC  = 0xA5;

static inline uint8_t bcd(uint8_t d)    { return uint8_t(((d/10)<<4) | (d%10)); }
static inline uint8_t debcd(uint8_t v)  { return uint8_t((v>>4)*10 + (v & 0x0F)); }

static bool plausible(const Time& t) {
    if (t.year < 2000 || t.year > 2099) return false;
    if (t.month < 1   || t.month > 12)  return false;
    if (t.day < 1     || t.day > 31)    return false;
    if (t.hour < 0    || t.hour > 23)   return false;
    if (t.minute < 0  || t.minute > 59) return false;
    if (t.second < 0  || t.second > 59) return false;
    return true;
}

bool begin(uint8_t i2c_addr) {
    g_addr = i2c_addr;
    // Update RTC_DEVICE address in case it differs from default
    i2c_manager::RTC_DEVICE.addr = g_addr;
    return true;
}

bool read(Time& t) {
    uint8_t buf[7];
    if (!i2c_manager::read(i2c_manager::RTC_DEVICE, REG_SECONDS, buf, sizeof(buf))) {
        t.valid = false;
        return false;
    }

    t.second = debcd(buf[0] & 0x7F);
    t.minute = debcd(buf[1] & 0x7F);
    t.hour   = debcd(buf[2] & 0x3F);
    t.day    = debcd(buf[3] & 0x3F);
    t.wday   = (buf[4] & 0x07);
    t.month  = debcd(buf[5] & 0x1F);
    t.year   = 2000 + debcd(buf[6]);

    t.valid = plausible(t) && is_initialized();
    return t.valid;
}

bool set(const Time& t) {
    if (!plausible(t)) return false;

    uint8_t out[7];
    out[0] = bcd(t.second);
    out[1] = bcd(t.minute);
    out[2] = bcd(t.hour);
    out[3] = bcd(t.day);
    out[4] = (uint8_t)(t.wday & 0x07);
    out[5] = bcd((uint8_t)t.month);
    out[6] = bcd((uint8_t)(t.year - 2000));

    if (!i2c_manager::write(i2c_manager::RTC_DEVICE, REG_SECONDS, out, sizeof(out))) {
        return false;
    }

    uint8_t magic = INIT_MAGIC;
    i2c_manager::write(i2c_manager::RTC_DEVICE, REG_RAM, &magic, 1);
    return true;
}

bool is_initialized() {
    uint8_t v = 0;
    if (!i2c_manager::readByte(i2c_manager::RTC_DEVICE, REG_RAM, v)) return false;
    return v == INIT_MAGIC;
}

void clear_initialized() {
    uint8_t z = 0;
    i2c_manager::write(i2c_manager::RTC_DEVICE, REG_RAM, &z, 1);
}

bool set_from_compile_time() {
    char mon[4]; int dd, yy; int hh, mm, ss;
    if (sscanf(__DATE__, "%3s %d %d", mon, &dd, &yy) != 3) return false;
    if (sscanf(__TIME__, "%d:%d:%d", &hh, &mm, &ss) != 3)   return false;

    static const char* names = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const char* p = strstr(names, mon);
    int mo = p ? (int)((p - names)/3 + 1) : 1;

    Time t{yy, mo, dd, hh, mm, ss, 0, true};
    return set(t);
}

bool set_from_epoch(uint32_t epoch, int tz_offset_minutes) {
    int64_t s = (int64_t)epoch + (int64_t)tz_offset_minutes * 60;
    if (s < 0) s = 0;
    int ss = s % 60; s /= 60;
    int mm = s % 60; s /= 60;
    int hh = s % 24; s /= 24;
    int64_t days = s;

    int y = 1970;
    auto is_leap = [&](int Y) { return (Y%4==0 && (Y%100!=0 || Y%400==0)); };
    while (true) {
        int dy = is_leap(y) ? 366 : 365;
        if (days >= dy) { days -= dy; y++; }
        else break;
    }
    int mdays[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (is_leap(y)) mdays[1] = 29;
    int m = 0;
    while (m < 12 && days >= mdays[m]) { days -= mdays[m]; m++; }
    int d = (int)days + 1;

    int wday = (int)((4 + (epoch/86400)) % 7);
    if (wday < 0) wday += 7;

    Time t{ y, m+1, d, hh, mm, ss, wday, true };
    return set(t);
}

} // namespace rtc
