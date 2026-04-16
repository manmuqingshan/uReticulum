#include "rtreticulum/os.h"

#include <cmath>

namespace RNS { namespace Utilities {

OS::LoopCallback OS::_on_loop = nullptr;

uint64_t OS::ltime() {
    return rt_hal_millis();
}

double OS::time() {
    uint64_t us = rt_hal_unix_micros();
    if (us != 0) return static_cast<double>(us) / 1000000.0;
    /* No RTC — fall back to uptime seconds. */
    return static_cast<double>(rt_hal_millis()) / 1000.0;
}

void OS::sleep(float seconds) {
    if (seconds <= 0.0f) return;
    rt_hal_delay_ms(static_cast<uint32_t>(seconds * 1000.0f));
}

double OS::round(double value, uint8_t precision) {
    if (precision == 0) return std::round(value);
    return std::round(value / precision) * precision;
}

void OS::reset_watchdog() {
    rt_hal_watchdog_feed();
}

void OS::set_loop_callback(LoopCallback on_loop) {
    _on_loop = on_loop;
}

void OS::run_loop() {
    if (_on_loop) _on_loop();
}

static inline bool is_big_endian() {
    uint16_t test = 0x0102;
    return reinterpret_cast<uint8_t*>(&test)[0] == 0x01;
}

static inline uint16_t swap16(uint16_t v) { return (v << 8) | (v >> 8); }
static inline uint32_t swap32(uint32_t v) {
    return ((v << 24) & 0xFF000000u) |
           ((v <<  8) & 0x00FF0000u) |
           ((v >>  8) & 0x0000FF00u) |
           ((v >> 24) & 0x000000FFu);
}

uint16_t OS::portable_htons(uint16_t v) { return is_big_endian() ? v : swap16(v); }
uint32_t OS::portable_htonl(uint32_t v) { return is_big_endian() ? v : swap32(v); }
uint16_t OS::portable_ntohs(uint16_t v) { return is_big_endian() ? v : swap16(v); }
uint32_t OS::portable_ntohl(uint32_t v) { return is_big_endian() ? v : swap32(v); }

uint64_t OS::from_bytes_big_endian(const uint8_t* data, size_t len) {
    uint64_t r = 0;
    for (size_t i = 0; i < len; ++i) r = (r << 8) | data[i];
    return r;
}

}}  // namespace RNS::Utilities
