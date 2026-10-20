#include "core/util.h"

#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <system_error>

namespace swiftdm {
namespace {

const char* kUnitSuffix[] = {"B", "KB", "MB", "GB", "TB", "PB"};

}  // namespace

std::string format_bytes(uint64_t bytes, int decimals) {
    if (decimals < 0) decimals = 0;
    if (decimals > 6) decimals = 6;
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1000.0 && unit < 5) {
        value /= 1000.0;
        ++unit;
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f %s", decimals, value, kUnitSuffix[unit]);
    return buf;
}

std::string format_speed(double bytes_per_sec, int decimals) {
    if (bytes_per_sec < 0) return "--";
    return format_bytes(static_cast<uint64_t>(bytes_per_sec + 0.5), decimals) + "/sec";
}

std::string format_duration(int64_t seconds) {
    if (seconds < 0) return "--";
    const int64_t h = seconds / 3600;
    const int64_t m = (seconds % 3600) / 60;
    const int64_t s = seconds % 60;
    char buf[64];
    if (h > 0) {
        std::snprintf(buf, sizeof(buf), "%lld hour %02lld min", (long long)h, (long long)m);
    } else if (m > 0) {
        std::snprintf(buf, sizeof(buf), "%lld min %02lld sec", (long long)m, (long long)s);
    } else {
        std::snprintf(buf, sizeof(buf), "%lld sec", (long long)s);
    }
    return buf;
}

int percent_of(uint64_t done, uint64_t total) {
    if (total == 0) return 0;
    const long double p = (100.0L * static_cast<long double>(done)) /
                          static_cast<long double>(total);
    long long v = static_cast<long long>(p + 0.5);
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    return static_cast<int>(v);
}

int64_t unix_now() { return static_cast<int64_t>(std::time(nullptr)); }

std::string trim(std::string_view s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return std::string(s.substr(b, e - b));
}

bool starts_with(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
}

bool ends_with(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}

std::string to_lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::vector<std::string> split(std::string_view s, char delim) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == delim) {
            out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    out.push_back(cur);
    return out;
}

std::string join(const std::vector<std::string>& parts, std::string_view sep) {
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) out += sep;
        out += parts[i];
    }
    return out;
}

std::string encode_url_for_request(std::string_view url) {
    constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(url.size());

    size_t component_start = 0;
    const size_t scheme_end = url.find("://");
    if (scheme_end != std::string_view::npos) {
        const size_t authority_end = url.find_first_of("/?#", scheme_end + 3);
        component_start = authority_end == std::string_view::npos ? url.size() : authority_end;
    }

    out.append(url.substr(0, component_start));
    for (size_t i = component_start; i < url.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(url[i]);
        if (c == '%' && i + 2 < url.size() &&
            std::isxdigit(static_cast<unsigned char>(url[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(url[i + 2]))) {
            out.append(url.substr(i, 3));
            i += 2;
            continue;
        }

        const bool unsafe = c <= 0x20 || c >= 0x7f || c == '%' || c == '"' || c == '<' ||
                            c == '>' || c == '`' || c == '{' || c == '|' || c == '}' ||
                            c == '\\' || c == '^';
        if (unsafe) {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 0x0f]);
        } else {
            out.push_back(static_cast<char>(c));
        }
    }
    return out;
}

std::string percent_decode(std::string_view text) {
    const auto hex_value = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };

    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size()) {
            const int high = hex_value(text[i + 1]);
            const int low = hex_value(text[i + 2]);
            if (high >= 0 && low >= 0) {
                out.push_back(static_cast<char>((high << 4) | low));
                i += 2;
                continue;
            }
        }
        out.push_back(text[i]);
    }
    return out;
}

std::string home_dir() {
    const char* h = std::getenv("HOME");
    return h ? h : "";
}

std::string expand_tilde(std::string_view path) {
    if (!path.empty() && path[0] == '~') {
        std::string home = home_dir();
        if (path.size() == 1) return home;
        if (path[1] == '/') return home + std::string(path.substr(1));
    }
    return std::string(path);
}

std::string xdg_config_home() {
    const char* c = std::getenv("XDG_CONFIG_HOME");
    if (c && *c) return c;
    return home_dir() + "/.config";
}

std::string xdg_data_home() {
    const char* c = std::getenv("XDG_DATA_HOME");
    if (c && *c) return c;
    return home_dir() + "/.local/share";
}

bool ensure_directory(const std::string& path, std::string* error) {
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    if (ec) {
        if (error) *error = "cannot create directory '" + path + "': " + ec.message();
        return false;
    }
    return true;
}

namespace {

int64_t file_time_to_unix(std::filesystem::file_time_type time) {
    const auto system_time = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        time - std::filesystem::file_time_type::clock::now() +
        std::chrono::system_clock::now());
    return static_cast<int64_t>(std::chrono::system_clock::to_time_t(system_time));
}

bool is_unique_suffix(const std::string& name, const std::string& stem,
                      const std::string& ext) {
    if (!starts_with(name, stem + " (")) return false;
    const size_t open = stem.size() + 2;
    const size_t close = name.find(')', open);
    if (close == std::string::npos) return false;
    if (close == open) return false;
    for (size_t i = open; i < close; ++i) {
        if (name[i] < '0' || name[i] > '9') return false;
    }
    return name.substr(close + 1) == ext;
}

}  // namespace

std::string resolve_existing_file(const std::string& dir, const std::string& filename,
                                  int64_t near_unix) {
    if (dir.empty() || filename.empty()) return {};
    std::error_code ec;
    const std::filesystem::path exact = std::filesystem::path(dir) / filename;
    if (std::filesystem::is_regular_file(exact, ec)) return exact.string();

    const std::filesystem::path base(filename);
    const std::string stem = base.stem().string();
    const std::string ext = base.extension().string();
    if (stem.empty()) return {};

    std::string best;
    int64_t best_distance = 0;
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end;
         it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const std::string name = it->path().filename().string();
        if (!is_unique_suffix(name, stem, ext)) continue;
        const int64_t written = file_time_to_unix(
            std::filesystem::last_write_time(it->path(), ec));
        if (ec) continue;
        const int64_t distance =
            near_unix > 0 ? std::llabs(written - near_unix) : 0;
        if (best.empty() || distance < best_distance) {
            best = it->path().string();
            best_distance = distance;
        }
    }
    return best;
}

std::string sanitize_filename(std::string_view name) {
    std::string out;
    out.reserve(name.size());
    for (char c : name) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' ||
            c == '<' || c == '>' || c == '|' || uc < 0x20) {
            out.push_back('_');
        } else {
            out.push_back(c);
        }
    }
    // Trim leading/trailing dots and spaces (Windows-style safety, harmless on Linux).
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
    size_t b = 0;
    while (b < out.size() && (out[b] == ' ')) ++b;
    out = out.substr(b);
    return out;
}

std::string file_name_from_url(std::string_view url) {
    // Strip query/fragment.
    std::string_view u = url;
    auto q = u.find_first_of("?#");
    if (q != std::string_view::npos) u = u.substr(0, q);
    // Strip trailing slashes.
    while (!u.empty() && u.back() == '/') u.remove_suffix(1);
    auto slash = u.find_last_of('/');
    std::string_view name = (slash == std::string_view::npos) ? u : u.substr(slash + 1);
    // Strip scheme prefix (e.g. "https:" with no path).
    auto colon = name.find(':');
    if (colon != std::string_view::npos) name = name.substr(colon + 1);
    return sanitize_filename(percent_decode(name));
}

std::string extension_of(std::string_view filename) {
    auto dot = filename.find_last_of('.');
    if (dot == std::string_view::npos) return "";
    return to_lower(filename.substr(dot + 1));
}

std::string errno_string(int err) { return std::strerror(err); }

}  // namespace swiftdm
