#include "net/http.h"

#include <curl/curl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "core/log.h"
#include "core/util.h"
#include "net/interstitial.h"

#include "net/http_internal.h"

namespace swiftdm::detail {

// ---------------------------------------------------------------------------
// Error body summaries
// ---------------------------------------------------------------------------

bool is_utf8_continuation(char c) {
    return (static_cast<unsigned char>(c) & 0xC0) == 0x80;
}

// Shortens to a UTF-8 character boundary and marks the cut with "...".
std::string truncate_utf8(const std::string& text, size_t max_bytes) {
    if (text.size() <= max_bytes) return text;
    size_t cut = max_bytes;
    while (cut > 0 && is_utf8_continuation(text[cut])) --cut;
    return text.substr(0, cut) + "...";
}

std::string collapse_whitespace(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    bool pending_space = false;
    for (char c : text) {
        const unsigned char u = static_cast<unsigned char>(c);
        const bool is_space = u == ' ' || u == '\t' || u == '\n' || u == '\r' || u == '\f' ||
                              u == '\v';
        if (is_space) {
            pending_space = !out.empty();
            continue;
        }
        if (pending_space) out += ' ';
        pending_space = false;
        out += c;
    }
    return out;
}

std::string strip_html_tags(const std::string& html) {
    std::string out;
    out.reserve(html.size());
    bool in_tag = false;
    for (char c : html) {
        if (c == '<') {
            in_tag = true;
            continue;
        }
        if (c == '>') {
            in_tag = false;
            out += ' ';
            continue;
        }
        if (!in_tag) out += c;
    }
    return out;
}

std::string html_title(const std::string& html) {
    const std::string lower = to_lower(html);
    const size_t open = lower.find("<title");
    if (open == std::string::npos) return {};
    const size_t open_end = lower.find('>', open);
    if (open_end == std::string::npos) return {};
    const size_t close = lower.find("</title", open_end);
    if (close == std::string::npos) return {};
    return strip_html_tags(html.substr(open_end + 1, close - open_end - 1));
}

// Removes <script>/<style> bodies; their contents are not a message.
std::string drop_html_blocks(const std::string& html, const char* tag) {
    const std::string lower = to_lower(html);
    const std::string open = std::string("<") + tag;
    const std::string close = std::string("</") + tag;
    std::string out;
    size_t pos = 0;
    while (true) {
        const size_t start = lower.find(open, pos);
        if (start == std::string::npos) break;
        out += html.substr(pos, start - pos);
        const size_t end = lower.find(close, start);
        if (end == std::string::npos) {
            pos = html.size();
            break;
        }
        const size_t end_tag = lower.find('>', end);
        pos = end_tag == std::string::npos ? html.size() : end_tag + 1;
    }
    out += html.substr(pos);
    return out;
}

// Reads a flat JSON string field ("message", "value"). Nested objects are not
// relevant for error payloads, so this stays a small scanner.
bool json_string_field(const std::string& body, const char* key, std::string* value) {
    const std::string pattern = std::string("\"") + key + "\"";
    size_t pos = 0;
    while ((pos = body.find(pattern, pos)) != std::string::npos) {
        size_t i = pos + pattern.size();
        while (i < body.size() && std::isspace(static_cast<unsigned char>(body[i]))) ++i;
        if (i >= body.size() || body[i] != ':') {
            pos = i;
            continue;
        }
        ++i;
        while (i < body.size() && std::isspace(static_cast<unsigned char>(body[i]))) ++i;
        if (i >= body.size() || body[i] != '"') {
            pos = i;
            continue;
        }
        ++i;
        std::string out;
        bool closed = false;
        while (i < body.size()) {
            const char c = body[i];
            if (c == '\\' && i + 1 < body.size()) {
                const char escaped = body[i + 1];
                if (escaped == 'n' || escaped == 't' || escaped == 'r') out += ' ';
                else if (escaped == '"' || escaped == '\\' || escaped == '/') out += escaped;
                i += 2;
                continue;
            }
            if (c == '"') {
                closed = true;
                ++i;
                break;
            }
            out += c;
            ++i;
        }
        if (closed && !trim(out).empty()) {
            *value = collapse_whitespace(out);
            return true;
        }
        pos = i;
    }
    return false;
}

// REST APIs (pixeldrain, most JSON back ends) report the reason in the body:
// {"success":false,"value":"hotlink_detected","message":"Hotlinking was ..."}.
std::string json_error_message(const std::string& body) {
    std::string message;
    std::string value;
    const bool has_message = json_string_field(body, "message", &message);
    const bool has_value = json_string_field(body, "value", &value);
    if (has_message && has_value) return value + ": " + message;
    if (has_message) return message;
    if (has_value) return value;
    return {};
}

std::string condense_error_body(const std::string& body, const std::string& content_type) {
    const std::string trimmed = trim(body);
    if (trimmed.empty()) return {};
    const std::string json = json_error_message(trimmed);
    if (!json.empty()) return truncate_utf8(json, kErrorDetailLimit);

    std::string text = trimmed;
    const std::string lower_type = to_lower(content_type);
    if (lower_type.find("html") != std::string::npos || trimmed.front() == '<') {
        const std::string title = html_title(trimmed);
        if (!trim(title).empty()) return truncate_utf8(collapse_whitespace(title), kErrorDetailLimit);
        text = drop_html_blocks(trimmed, "script");
        text = drop_html_blocks(text, "style");
        text = strip_html_tags(text);
    }
    const std::string flat = collapse_whitespace(text);
    if (flat.empty()) return {};
    return truncate_utf8(flat, kErrorDetailLimit);
}

std::string http_error_with_detail(long code, const std::string& body,
                                   const std::string& content_type) {
    std::string text = http_error_text(code);
    const std::string detail = condense_error_body(body, content_type);
    if (detail.empty()) return text;
    return text + " - " + detail;
}

}  // namespace swiftdm::detail
