#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
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

// Encode characters that cannot safely appear literally in a request URL,
// preserving existing percent escapes and URL delimiters.
std::string encode_url_for_request(std::string_view url);
std::string percent_decode(std::string_view text);

// Filesystem helpers.
std::string home_dir();                       // $HOME on POSIX, %USERPROFILE% on Windows
std::string expand_tilde(std::string_view path);
std::string xdg_config_home();                // platform configuration root
std::string xdg_data_home();                  // platform data root
std::filesystem::path path_from_utf8(std::string_view path);
std::string path_to_utf8(const std::filesystem::path& path);
bool ensure_directory(const std::string& path, std::string* error = nullptr);
// Creates a private runtime/configuration directory (owner-only, mode 0700),
// tightening an existing directory as well.
bool ensure_private_directory(const std::string& path, std::string* error = nullptr);

// Replaces a file atomically using a same-directory temporary file and the
// requested mode (subject to no umask restriction after fchmod).
bool write_file_atomic(const std::string& path, std::string_view content,
                       unsigned int mode, std::string* error = nullptr);

// Directory of the running executable, without a trailing separator. Empty
// when it cannot be determined (for example /proc/self/exe is missing).
std::string executable_dir();

// CA bundle for libcurl, resolved once per process:
//   1. `env_override` when it names an existing file;
//   2. `ca-bundle.crt` next to the executable (the Windows package ships one);
//   3. empty, so libcurl keeps its built-in default (Linux).
// A missing or unreadable file is never returned: passing it to libcurl would
// trade the current handshake failure for CURLE_SSL_CACERT_BADFILE.
std::string resolve_ca_bundle(const std::string& exe_dir, std::string_view env_override);

// Convenience overload reading executable_dir() and the SWIFTDM_CA_BUNDLE
// environment variable.
std::string resolve_ca_bundle();

// Removes a file or a directory tree. Prefer this over
// std::filesystem::remove_all in portable code: MinGW's recursive iterator can
// spin forever on a directory that contains a name longer than MAX_PATH.
bool remove_tree(const std::string& path);

// Returns the path of an existing file for a completed download. The exact
// `dir/filename` wins; when it is gone, "name (N).ext" siblings are tried and
// the one whose modification time is closest to `near_unix` is chosen (that
// covers downloads saved with a unique suffix before the final name was
// persisted). Returns an empty string when no file matches.
std::string resolve_existing_file(const std::string& dir, const std::string& filename,
                                  int64_t near_unix = 0);

// File name utilities.
std::string sanitize_filename(std::string_view name);
std::string file_name_from_url(std::string_view url);
std::string extension_of(std::string_view filename);

// Maximum length of a single path component. Linux filesystems limit a
// component to NAME_MAX (255 bytes) and Windows to 255 UTF-16 code units.
// The number of UTF-16 code units never exceeds the number of UTF-8 bytes,
// so 240 UTF-8 bytes fits both platforms.
inline constexpr size_t kMaxFileNameBytes = 240;

// Longest extension kept when a name has to be shortened.
inline constexpr size_t kMaxExtensionBytes = 16;

// Cuts `text` to at most `bytes` bytes without splitting a UTF-8 sequence.
std::string_view utf8_truncate(std::string_view text, size_t bytes);

// Shortens `name` so that it fits into `max_bytes` bytes. The extension is
// kept and a hash of the original name is appended: two different long names
// never collapse into the same file, and the result is stable across runs so
// a resumed download keeps the same temporary file.
std::string limit_filename_length(std::string_view name,
                                  size_t max_bytes = kMaxFileNameBytes);

// True for the device names Windows reserves (CON, PRN, AUX, NUL, COM1-9,
// LPT1-9 and their superscript variants), with or without an extension.
bool is_windows_reserved_name(std::string_view name);

// FNV-1a 64-bit hash and its hexadecimal form, used for the workspace
// directory names and for the hash appended to a shortened file name.
uint64_t fnv1a_hash(std::string_view text);
std::string hex_hash64(uint64_t value);

// Errors as strings.
std::string errno_string(int err);

}  // namespace swiftdm
