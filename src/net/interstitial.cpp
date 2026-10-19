#include "net/interstitial.h"

#include <cctype>
#include <cstddef>
#include <initializer_list>
#include <string>
#include <vector>

#include "core/util.h"

namespace swiftdm {
namespace {

// Drive ids and the confirmation tokens are short opaque tokens; anything else
// (a path segment such as "view", a placeholder filled by script) is ignored.
constexpr size_t kMaxIdLength = 64;

std::string host_of(const std::string& url) {
    const size_t scheme = url.find("://");
    if (scheme == std::string::npos) return {};
    size_t begin = scheme + 3;
    const size_t end = url.find_first_of("/?#", begin);
    std::string host =
        url.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
    const size_t at = host.rfind('@');
    if (at != std::string::npos) host = host.substr(at + 1);
    const size_t colon = host.find(':');
    if (colon != std::string::npos) host = host.substr(0, colon);
    return to_lower(host);
}

bool is_one_of(const std::string& host, std::initializer_list<const char*> hosts) {
    for (const char* candidate : hosts) {
        if (host == candidate) return true;
    }
    return false;
}

std::string path_of(const std::string& url) {
    const size_t scheme = url.find("://");
    const size_t begin = scheme == std::string::npos ? 0 : scheme + 3;
    const size_t slash = url.find('/', begin);
    if (slash == std::string::npos) return {};
    const size_t end = url.find_first_of("?#", slash);
    return url.substr(slash, end == std::string::npos ? std::string::npos : end - slash);
}

std::string query_of(const std::string& url) {
    const size_t q = url.find('?');
    if (q == std::string::npos) return {};
    const size_t end = url.find('#', q);
    return url.substr(q + 1, end == std::string::npos ? std::string::npos : end - q - 1);
}

bool looks_like_id(const std::string& value) {
    if (value.empty() || value.size() > kMaxIdLength) return false;
    for (char c : value) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-') return false;
    }
    return true;
}

// "<marker><id>[/rest]" -> id (validated), otherwise "". "marker" may appear
// after an account segment ("/drive/u/<n>/folders/<id>").
std::string segment_after(const std::string& path, const char* marker) {
    const std::string head = marker;
    const size_t pos = path.find(head);
    if (pos == std::string::npos) return {};
    const std::string rest = path.substr(pos + head.size());
    const size_t slash = rest.find('/');
    const std::string id = slash == std::string::npos ? rest : rest.substr(0, slash);
    return looks_like_id(id) ? id : std::string();
}

std::string percent_encode(const std::string& text) {
    static const char* kHex = "0123456789ABCDEF";
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (std::isalnum(u) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += c;
            continue;
        }
        out += '%';
        out += kHex[u >> 4];
        out += kHex[u & 0x0f];
    }
    return out;
}

// Minimal entity decoding for attribute values and URLs found in a page.
std::string html_unescape(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    size_t i = 0;
    while (i < text.size()) {
        if (text[i] != '&') {
            out += text[i++];
            continue;
        }
        const size_t semi = text.find(';', i);
        if (semi == std::string::npos || semi - i > 8) {
            out += text[i++];
            continue;
        }
        const std::string entity = to_lower(text.substr(i + 1, semi - i - 1));
        if (entity == "amp") out += '&';
        else if (entity == "quot") out += '"';
        else if (entity == "apos" || entity == "#39") out += '\'';
        else if (entity == "lt") out += '<';
        else if (entity == "gt") out += '>';
        else if (entity == "nbsp") out += ' ';
        else {
            out += text[i++];
            continue;
        }
        i = semi + 1;
    }
    return out;
}

// Value of an attribute inside a single tag ("name=\"id\""); attributes are
// case-insensitive and may be quoted with ' or " or left bare.
bool attr_value(const std::string& tag, const char* name, std::string* out) {
    const std::string lower = to_lower(tag);
    const std::string key = name;
    size_t pos = 0;
    while ((pos = lower.find(key, pos)) != std::string::npos) {
        const bool boundary = pos == 0 || std::isspace(static_cast<unsigned char>(lower[pos - 1])) ||
                              lower[pos - 1] == '<';
        size_t i = pos + key.size();
        pos = i;
        if (!boundary) continue;
        while (i < lower.size() && std::isspace(static_cast<unsigned char>(lower[i]))) ++i;
        if (i >= lower.size() || lower[i] != '=') continue;
        ++i;
        while (i < lower.size() && std::isspace(static_cast<unsigned char>(lower[i]))) ++i;
        if (i >= tag.size()) return false;
        const char quote = tag[i];
        if (quote == '"' || quote == '\'') {
            const size_t end = tag.find(quote, i + 1);
            if (end == std::string::npos) return false;
            *out = tag.substr(i + 1, end - i - 1);
            return true;
        }
        size_t end = i;
        while (end < tag.size() && !std::isspace(static_cast<unsigned char>(tag[end])) &&
               tag[end] != '>') {
            ++end;
        }
        *out = tag.substr(i, end - i);
        return true;
    }
    return false;
}

}  // namespace

bool is_google_drive_host(const std::string& url) {
    return is_one_of(host_of(url),
                     {"drive.google.com", "drive.usercontent.google.com", "drive.googleusercontent.com"});
}

bool is_google_docs_host(const std::string& url) {
    return is_one_of(host_of(url), {"docs.google.com"});
}

bool is_google_accounts_host(const std::string& url) {
    return is_one_of(host_of(url), {"accounts.google.com"});
}

bool is_google_signin_page(const std::string& html) {
    const std::string lower = to_lower(html);
    const bool google_signin = lower.find("accounts.google.com") != std::string::npos ||
                               lower.find("service=wise") != std::string::npos ||
                               lower.find("identifierid") != std::string::npos;
    if (!google_signin) return false;
    return lower.find("signin") != std::string::npos ||
           lower.find("sign in") != std::string::npos ||
           lower.find("sign-in") != std::string::npos;
}

std::string google_drive_file_id(const std::string& url) {
    if (!is_google_drive_host(url)) return {};
    if (const std::string id = segment_after(path_of(url), "/file/d/"); !id.empty()) return id;
    const std::string param = url_query_param(url, "id");
    return looks_like_id(param) ? param : std::string();
}

std::string google_drive_folder_id(const std::string& url) {
    if (!is_google_drive_host(url)) return {};
    return segment_after(path_of(url), "/folders/");
}

std::string google_drive_download_url(const std::string& file_id) {
    if (!looks_like_id(file_id)) return {};
    return "https://drive.usercontent.google.com/download?id=" + file_id + "&export=download";
}

std::string keep_drive_handoff_params(const std::string& url, const std::string& link) {
    std::string out = url;
    for (const char* key : {"authuser", "resourcekey", "confirm"}) {
        if (!url_query_param(out, key).empty()) continue;
        const std::string value = url_query_param(link, key);
        if (value.empty() || value.size() > kMaxIdLength) continue;
        out += "&";
        out += key;
        out += "=";
        out += percent_encode(value);
    }
    return out;
}

std::string url_query_param(const std::string& url, const std::string& name) {
    const std::string query = query_of(url);
    const std::string key = name + "=";
    size_t pos = 0;
    while (pos < query.size()) {
        const size_t end = query.find('&', pos);
        const std::string pair =
            query.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        if (pair.rfind(key, 0) == 0) return pair.substr(key.size());
        if (end == std::string::npos) break;
        pos = end + 1;
    }
    return {};
}

std::string page_error_reason(const std::string& url, const std::string& final_url) {
    if (is_google_drive_host(url) || is_google_drive_host(final_url)) return kDrivePageError;
    if (is_google_docs_host(url) || is_google_docs_host(final_url)) return kDocsEditorError;
    return {};
}

std::string signin_error_reason(const std::string& effective_url, const std::string& html) {
    if (is_google_accounts_host(effective_url) || is_google_signin_page(html)) return kSignInError;
    return {};
}

std::string download_confirmation_url(const std::string& html) {
    const std::string lower = to_lower(html);
    size_t pos = 0;
    while ((pos = lower.find("<form", pos)) != std::string::npos) {
        const size_t tag_end = html.find('>', pos);
        if (tag_end == std::string::npos) break;
        const size_t close = lower.find("</form", tag_end);
        const size_t block_end = close == std::string::npos ? html.size() : close;

        std::string action;
        if (attr_value(html.substr(pos, tag_end - pos + 1), "action", &action)) {
            std::vector<std::pair<std::string, std::string>> fields;
            bool has_id = false;
            bool has_confirm = false;
            size_t input_pos = tag_end + 1;
            while (input_pos < block_end) {
                const size_t start = lower.find("<input", input_pos);
                if (start == std::string::npos || start >= block_end) break;
                const size_t input_end = html.find('>', start);
                if (input_end == std::string::npos) break;
                const std::string tag = html.substr(start, input_end - start + 1);
                input_pos = input_end + 1;

                std::string name;
                if (!attr_value(tag, "name", &name)) continue;  // unnamed inputs submit nothing
                std::string value;
                attr_value(tag, "value", &value);
                const std::string lname = to_lower(name);
                if (lname == "id") has_id = true;
                if (lname == "confirm") has_confirm = true;
                // The session token is dropped: Drive does not validate it and
                // the URL has to stay stable for resumed partial files.
                if (lname == "uuid") continue;
                fields.emplace_back(name, html_unescape(value));
            }
            if (has_id && has_confirm) {
                std::string url = html_unescape(trim(action));
                for (const auto& [name, value] : fields) {
                    if (name.empty() || value.empty()) continue;
                    url += (url.find('?') == std::string::npos ? "?" : "&");
                    url += percent_encode(name) + "=" + percent_encode(value);
                }
                return url;
            }
        }
        pos = block_end;
    }
    return {};
}

}  // namespace swiftdm
