#pragma once

#include <string>
#include <string_view>

namespace swiftdm {

enum class LogLevel { Trace, Debug, Info, Warn, Error };

// Minimal stderr logger. Kept dependency-free so the CLI can be tested
// headlessly per AGENTS.md rules.
void log_set_level(LogLevel level);
void log_write(LogLevel level, std::string_view message);

void log_info(std::string_view message);
void log_warn(std::string_view message);
void log_error(std::string_view message);

std::string format_seconds(double seconds);  // "1m 23s" style helper

}  // namespace swiftdm
