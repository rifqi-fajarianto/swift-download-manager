#include "engine/hls.h"

#include <cctype>
#include <cstdlib>

namespace swiftdm {
namespace {

std::string lower_copy(const std::string& text) {
    std::string out(text);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

std::string trim_copy(const std::string& text) {
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
    return text.substr(begin, end - begin);
}

// Extracts `NAME=value` (or `NAME="value"`) from a tag line. Only attribute
// boundaries (start, ':' or ',') match, so `URI=` does not match `X-URI=`.
std::string attribute_value(const std::string& line, const std::string& name) {
    const std::string needle = name + "=";
    size_t pos = 0;
    while ((pos = line.find(needle, pos)) != std::string::npos) {
        const bool boundary = pos == 0 || line[pos - 1] == ':' || line[pos - 1] == ',';
        if (boundary) {
            size_t begin = pos + needle.size();
            if (begin < line.size() && line[begin] == '"') {
                ++begin;
                const size_t end = line.find('"', begin);
                if (end == std::string::npos) return {};
                return line.substr(begin, end - begin);
            }
            const size_t end = line.find(',', begin);
            return trim_copy(line.substr(begin, end == std::string::npos
                                                    ? std::string::npos
                                                    : end - begin));
        }
        pos += needle.size();
    }
    return {};
}

std::string scheme_and_authority(const std::string& url) {
    const size_t scheme_end = url.find("://");
    if (scheme_end == std::string::npos) return {};
    const size_t path_start = url.find('/', scheme_end + 3);
    return path_start == std::string::npos ? url : url.substr(0, path_start);
}

std::string directory_of(const std::string& url) {
    const size_t scheme_end = url.find("://");
    const size_t slash = url.rfind('/');
    if (slash == std::string::npos ||
        (scheme_end != std::string::npos && slash < scheme_end + 3)) {
        return url + "/";
    }
    return url.substr(0, slash + 1);
}

}  // namespace

bool parse_hls_playlist(const std::string& text, HlsPlaylist* out, std::string* error) {
    if (!out) {
        if (error) *error = "no playlist output buffer";
        return false;
    }
    *out = HlsPlaylist{};

    bool saw_header = false;
    bool expect_variant = false;
    bool expect_segment = false;
    int64_t variant_bandwidth = 0;
    size_t pos = 0;
    while (pos <= text.size()) {
        const size_t eol = text.find('\n', pos);
        std::string line = text.substr(pos, eol == std::string::npos ? std::string::npos
                                                                     : eol - pos);
        pos = eol == std::string::npos ? text.size() + 1 : eol + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::string stripped = trim_copy(line);
        if (stripped.empty()) continue;

        if (stripped[0] == '#') {
            if (!saw_header) {
                if (stripped.rfind("#EXTM3U", 0) != 0) {
                    if (error) *error = "not an m3u8 playlist";
                    return false;
                }
                saw_header = true;
                continue;
            }
            if (stripped.rfind("#EXT-X-STREAM-INF:", 0) == 0) {
                out->master = true;
                const std::string bandwidth = attribute_value(stripped, "BANDWIDTH");
                variant_bandwidth =
                    bandwidth.empty() ? 0 : std::strtoll(bandwidth.c_str(), nullptr, 10);
                expect_variant = true;
            } else if (stripped.rfind("#EXTINF:", 0) == 0) {
                expect_segment = true;
            } else if (stripped.rfind("#EXT-X-MAP:", 0) == 0) {
                out->init_segment = attribute_value(stripped, "URI");
            } else if (stripped.rfind("#EXT-X-KEY:", 0) == 0) {
                const std::string method = attribute_value(stripped, "METHOD");
                if (!method.empty() && method != "NONE") out->encrypted = true;
            }
            continue;
        }

        if (!saw_header) {
            if (error) *error = "not an m3u8 playlist";
            return false;
        }
        if (expect_variant) {
            out->variants.emplace_back(variant_bandwidth, stripped);
            expect_variant = false;
        } else if (expect_segment) {
            out->segments.push_back(stripped);
            expect_segment = false;
        }
    }

    if (!saw_header) {
        if (error) *error = "empty playlist";
        return false;
    }
    if (out->master && out->variants.empty()) {
        if (error) *error = "master playlist has no variants";
        return false;
    }
    if (!out->master && out->segments.empty() && !out->encrypted) {
        if (error) *error = "media playlist has no segments";
        return false;
    }
    if (error) error->clear();
    return true;
}

std::string resolve_playlist_uri(const std::string& base_url, const std::string& uri) {
    if (uri.empty()) return uri;
    if (uri.find("://") != std::string::npos) return uri;
    if (uri.rfind("//", 0) == 0) {
        const size_t scheme_end = base_url.find("://");
        if (scheme_end == std::string::npos) return uri;
        return base_url.substr(0, scheme_end) + ":" + uri;
    }
    if (uri[0] == '/') {
        const std::string authority = scheme_and_authority(base_url);
        if (authority.empty()) return uri;
        return authority + uri;
    }
    return directory_of(base_url) + uri;
}

bool is_hls_url(const std::string& url, const std::string& mime_type) {
    const std::string mime = lower_copy(mime_type);
    if (mime.find("mpegurl") != std::string::npos) return true;

    std::string path = url;
    const size_t query = path.find_first_of("?#");
    if (query != std::string::npos) path = path.substr(0, query);
    const std::string lowered = lower_copy(path);
    return lowered.size() > 5 && lowered.rfind(".m3u8") == lowered.size() - 5;
}

}  // namespace swiftdm
