#include "rtreticulum/log.h"

#include <stdio.h>
#include <string.h>

#include "rtreticulum/hal.h"
#include "rtreticulum/os.h"

namespace {
    RNS::LogLevel    g_level   = RNS::LOG_TRACE;
    RNS::log_callback g_on_log = nullptr;
    char              g_datetime[24];
}

const char* RNS::getLevelName(LogLevel level) {
    switch (level) {
    case LOG_CRITICAL: return "!!!";
    case LOG_ERROR:    return "ERR";
    case LOG_WARNING:  return "WRN";
    case LOG_NOTICE:   return "NOT";
    case LOG_INFO:     return "INF";
    case LOG_VERBOSE:  return "VRB";
    case LOG_DEBUG:    return "DBG";
    case LOG_TRACE:    return "---";
    case LOG_MEM:      return "...";
    default:           return "   ";
    }
}

const char* RNS::getTimeString() {
    uint64_t t = Utilities::OS::ltime();
    if (t < 86400000ULL) {
        snprintf(g_datetime, sizeof(g_datetime), "%02u:%02u:%02u.%03u",
                 (unsigned)(t / 3600000ULL),
                 (unsigned)((t / 60000ULL) % 60),
                 (unsigned)((t / 1000ULL) % 60),
                 (unsigned)(t % 1000ULL));
    } else {
        snprintf(g_datetime, sizeof(g_datetime), "%02u-%02u:%02u:%02u.%03u",
                 (unsigned)(t / 86400000ULL),
                 (unsigned)((t / 3600000ULL) % 24),
                 (unsigned)((t / 60000ULL) % 60),
                 (unsigned)((t / 1000ULL) % 60),
                 (unsigned)(t % 1000ULL));
    }
    return g_datetime;
}

void     RNS::loglevel(LogLevel level) { g_level = level; }
RNS::LogLevel RNS::loglevel()          { return g_level; }

void RNS::set_log_callback(log_callback on_log) { g_on_log = on_log; }

void RNS::doLog(LogLevel level, const char* msg) {
    if (level > g_level) return;
    if (g_on_log != nullptr) { g_on_log(msg, level); return; }

    char line[RNS_LOG_BUFFER_SIZE];
    int n = snprintf(line, sizeof(line), "%s [%s] %s\n",
                     getTimeString(), getLevelName(level), msg);
    if (n < 0) return;
    if ((size_t)n >= sizeof(line)) n = sizeof(line) - 1;
    rt_hal_log_write(line, (size_t)n);
}

void RNS::doHeadLog(LogLevel level, const char* msg) {
    if (level > g_level) return;
    if (g_on_log != nullptr) {
        g_on_log("", level);
        g_on_log(msg, level);
        return;
    }
    rt_hal_log_write("\n", 1);
    doLog(level, msg);
}
