#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace swiftdm {

// Human-readable size using decimal units (matches the original app's
// "100.000 MB" style): B, KB, MB, GB, TB. decimals controls fraction digits.
std::string format_bytes(uint64_t bytes, int decimals = 3);

// Speed string like "27.653 MB/sec" (decimal units).
std::string format_speed(double bytes_per_sec, int decimals = 3);

// Short duration string: "1 sec", "5 min 03 sec", "2 hour 05 min".
std::string format_duration(int64_t seconds);

// Percentage as integer: "54" for 54.2%.
int percent_of(uint64_t done, uint64_t total);

// Timestamp helpers (unix seconds).
int64_t unix_now();

// String helpers.
std::string trim(std::string_view s);
bool starts_with(std::string_view s, std::string_view prefix);
bool ends_with(std::string_view s, std::string_view suffix);
std::string to_lower(std::string_view s);
std::vector<std::string> split(std::string_view s, char delim);
std::string join(const std::vector<std::string>& parts, std::string_view sep);

// Filesystem helpers.
std::string home_dir();                       // $HOME or empty
std::string expand_tilde(std::string_view path);
std::string xdg_config_home();                // $XDG_CONFIG_HOME or ~/.config
std::string xdg_data_home();                  // $XDG_DATA_HOME or ~/.local/share
bool ensure_directory(const std::string& path, std::string* error = nullptr);

// File name utilities.
std::string sanitize_filename(std::string_view name);
std::string file_name_from_url(std::string_view url);
std::string extension_of(std::string_view filename);

// Errors as strings.
std::string errno_string(int err);

}  // namespace swiftdm
