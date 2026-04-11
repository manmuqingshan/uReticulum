#pragma once

#include <stdint.h>

#include <functional>

#include "ureticulum/hal.h"

/* Subset of microReticulum's RNS::Utilities::OS that core code depends on.
 * FileSystem surface is deliberately excluded — it lives behind its own
 * injected interface so storage-free targets don't have to stub it. */

namespace RNS { namespace Utilities {

    class OS {
    public:
        using LoopCallback = std::function<void()>;

        static uint64_t ltime();
        static double   time();
        static void     sleep(float seconds);
        static double   round(double value, uint8_t precision);
        static void     reset_watchdog();

        static void set_loop_callback(LoopCallback on_loop = nullptr);
        static void run_loop();

        static uint16_t portable_htons(uint16_t v);
        static uint32_t portable_htonl(uint32_t v);
        static uint16_t portable_ntohs(uint16_t v);
        static uint32_t portable_ntohl(uint32_t v);

        static uint64_t from_bytes_big_endian(const uint8_t* data, size_t len);

    private:
        static LoopCallback _on_loop;
    };

}}
