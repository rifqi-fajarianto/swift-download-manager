#include "core/util.h"

#include <cctype>
#include <cstdio>
#include <ctime>
#include <string>
#include <string_view>
#include <vector>

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

}  // namespace swiftdm
