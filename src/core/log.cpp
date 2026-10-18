#include "core/log.h"

#include <cstdio>

namespace swiftdm {
namespace {
LogLevel g_level = LogLevel::Info;

const char* level_name(LogLevel level) {
    switch (level) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO";
        case LogLevel::Warn:  return "WARN";
        case LogLevel::Error: return "ERROR";
    }
    return "?";
}
}  // namespace

void log_set_level(LogLevel level) { g_level = level; }

void log_write(LogLevel level, std::string_view message) {
    if (static_cast<int>(level) < static_cast<int>(g_level)) return;
    std::fprintf(stderr, "[%s] %.*s\n", level_name(level),
                 static_cast<int>(message.size()), message.data());
}

void log_info(std::string_view message) { log_write(LogLevel::Info, message); }
void log_warn(std::string_view message) { log_write(LogLevel::Warn, message); }
void log_error(std::string_view message) { log_write(LogLevel::Error, message); }

std::string format_seconds(double seconds) {
    if (seconds < 0) return "--";
    const long total = static_cast<long>(seconds + 0.5);
    const long h = total / 3600;
    const long m = (total % 3600) / 60;
    const long s = total % 60;
    char buf[32];
    if (h > 0) {
        std::snprintf(buf, sizeof(buf), "%ldh %02ldm %02lds", h, m, s);
    } else if (m > 0) {
        std::snprintf(buf, sizeof(buf), "%ldm %02lds", m, s);
    } else {
        std::snprintf(buf, sizeof(buf), "%lds", s);
    }
    return buf;
}

}  // namespace swiftdm
