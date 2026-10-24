#include "net/http.h"

#include <curl/curl.h>
#include <unistd.h>

#include <algorithm>
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

namespace swiftdm {

std::string http_reason_phrase(long status) {
    switch (status) {
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 206: return "Partial Content";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 303: return "See Other";
        case 304: return "Not Modified";
        case 307: return "Temporary Redirect";
        case 308: return "Permanent Redirect";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 409: return "Conflict";
        case 410: return "Gone";
        case 413: return "Payload Too Large";
        case 416: return "Requested Range Not Satisfiable";
        case 429: return "Too Many Requests";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        case 504: return "Gateway Timeout";
        default: return "";
    }
}

int http_error_status(const std::string& error) {
    constexpr const char* kPrefix = "HTTP/1.1 ";
    constexpr size_t kPrefixLen = 9;  // strlen(kPrefix)
    if (error.rfind(kPrefix, 0) != 0 || error.size() <= kPrefixLen) return 0;
    if (!std::isdigit(static_cast<unsigned char>(error[kPrefixLen]))) return 0;
    return static_cast<int>(std::strtol(error.c_str() + kPrefixLen, nullptr, 10));
}

// One allowlist for every URL that reaches libcurl: the probe, HLS playlists
// (whose segment URIs come from a remote file), sequential transfers and
// range segments. libcurl would otherwise accept schemes such as file://,
// which would let a page read a local file into the download.
bool is_supported_download_url(const std::string& url) {
    const size_t scheme_end = url.find("://");
    if (scheme_end == std::string::npos) return false;
    const std::string scheme = to_lower(url.substr(0, scheme_end));
    if (scheme != "http" && scheme != "https" && scheme != "ftp" && scheme != "ftps") {
        return false;
    }

    const size_t authority_start = scheme_end + 3;
    const size_t authority_end = url.find_first_of("/?#", authority_start);
    std::string authority = url.substr(
        authority_start, authority_end == std::string::npos ? std::string::npos
                                                            : authority_end - authority_start);
    const size_t userinfo_end = authority.rfind('@');
    if (userinfo_end != std::string::npos) authority.erase(0, userinfo_end + 1);
    if (authority.empty() || authority.find_first_of(" \t\r\n") != std::string::npos) return false;

    if (authority.front() == '[') {
        const size_t bracket = authority.find(']');
        return bracket != std::string::npos && bracket > 1;
    }
    const size_t colon = authority.find(':');
    if (colon != std::string::npos) authority.resize(colon);
    return !authority.empty();
}

namespace {

constexpr int kIoTimeoutMs = 100;         // curl multi poll timeout
constexpr int64_t kReportIntervalMs = 250;
constexpr int kMaxRetries = 5;

int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

void ensure_curl_global() {
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

std::string http_error_text(long code) {
    std::string text = "HTTP/1.1 " + std::to_string(code);
    const std::string reason = http_reason_phrase(code);
    if (!reason.empty()) text += " " + reason;
    return text;
}

std::string curl_url_error(CURLcode code) {
    return code == CURLE_URL_MALFORMAT ? std::string(kInvalidUrlError)
                                       : std::string(curl_easy_strerror(code));
}

// Failure messages carry the host's own explanation (rate limits, captcha
// requirements, hotlink protection) instead of a bare "HTTP/1.1 403 Forbidden".
constexpr size_t kErrorBodyLimit = 1024;   // body bytes kept per request
constexpr size_t kErrorDetailLimit = 300;  // bytes shown in the message

curl_slist* build_headers(const std::vector<HttpHeader>& headers) {
    curl_slist* list = nullptr;
    for (const HttpHeader& h : headers) {
        if (h.name.empty()) continue;
        list = curl_slist_append(list, (h.name + ": " + h.value).c_str());
    }
    return list;
}

void apply_common_options(CURL* handle, const HttpOptions& o, curl_slist* headers) {
    const std::string request_url = encode_url_for_request(o.url);
    curl_easy_setopt(handle, CURLOPT_URL, request_url.c_str());
    curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
    // Defence in depth next to is_supported_download_url(): libcurl never
    // opens a scheme outside this list, including on redirect targets.
    // CURLOPT_PROTOCOLS_STR needs libcurl 7.85 (Debian 13 ships 8.x).
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(handle, CURLOPT_PROTOCOLS_STR, "http,https,ftp,ftps");
    curl_easy_setopt(handle, CURLOPT_REDIR_PROTOCOLS_STR, "http,https,ftp,ftps");
#else
    curl_easy_setopt(handle, CURLOPT_PROTOCOLS,
                     static_cast<long>(CURLPROTO_HTTP | CURLPROTO_HTTPS | CURLPROTO_FTP |
                                       CURLPROTO_FTPS));
    curl_easy_setopt(handle, CURLOPT_REDIR_PROTOCOLS,
                     static_cast<long>(CURLPROTO_HTTP | CURLPROTO_HTTPS | CURLPROTO_FTP |
                                       CURLPROTO_FTPS));
#endif
    curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(handle, CURLOPT_MAXREDIRS, 15L);
    curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT,
                     static_cast<long>(o.connect_timeout_seconds));
    curl_easy_setopt(handle, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(handle, CURLOPT_LOW_SPEED_TIME,
                     static_cast<long>(o.low_speed_seconds));
    if (!o.user_agent.empty()) curl_easy_setopt(handle, CURLOPT_USERAGENT, o.user_agent.c_str());
    if (!o.referer.empty()) curl_easy_setopt(handle, CURLOPT_REFERER, o.referer.c_str());
    if (headers) curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers);

    if (!o.cookies.empty()) {
        // Enable the cookie engine and seed URL-scoped cookies. libcurl then
        // selects the matching domain/path cookies on each redirect target.
        curl_easy_setopt(handle, CURLOPT_COOKIEFILE, "");
        bool cookie_setup_ok = true;
        for (const HttpCookie& cookie : o.cookies) {
            const bool include_subdomains = !cookie.domain.empty() && cookie.domain.front() == '.';
            std::string domain = cookie.domain;
            if (!domain.empty() && domain.front() == '.') domain.erase(domain.begin());
            if (domain.empty()) continue;
            const std::string line = domain + "\t" +
                                     (include_subdomains ? "TRUE" : "FALSE") + "\t" +
                                     (cookie.path.empty() ? "/" : cookie.path) + "\t" +
                                     (cookie.secure ? "TRUE" : "FALSE") + "\t" +
                                     std::to_string(cookie.expires) + "\t" + cookie.name +
                                     "\t" + cookie.value;
            if (curl_easy_setopt(handle, CURLOPT_COOKIELIST, line.c_str()) != CURLE_OK) {
                cookie_setup_ok = false;
                break;
            }
        }
        if (!cookie_setup_ok) curl_easy_setopt(handle, CURLOPT_COOKIEFILE, nullptr);
    }

    if (o.proxy_type == "http") {
        curl_easy_setopt(handle, CURLOPT_PROXYTYPE, CURLPROXY_HTTP);
    } else if (o.proxy_type == "socks4") {
        curl_easy_setopt(handle, CURLOPT_PROXYTYPE, CURLPROXY_SOCKS4);
    } else if (o.proxy_type == "socks5") {
        curl_easy_setopt(handle, CURLOPT_PROXYTYPE, CURLPROXY_SOCKS5);
    }
    if (o.proxy_type != "none" && !o.proxy_type.empty() && !o.proxy_address.empty()) {
        std::string proxy = o.proxy_address;
        if (o.proxy_port > 0) proxy += ":" + std::to_string(o.proxy_port);
        curl_easy_setopt(handle, CURLOPT_PROXY, proxy.c_str());
        if (!o.proxy_user.empty()) {
            const std::string credentials = o.proxy_user + ":" + o.proxy_password;
            curl_easy_setopt(handle, CURLOPT_PROXYUSERPWD, credentials.c_str());
        }
    }
}

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string percent_decode(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '%' && i + 2 < in.size()) {
            const int hi = hex_value(in[i + 1]);
            const int lo = hex_value(in[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out += static_cast<char>((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        out += in[i];
    }
    return out;
}

std::string parse_content_disposition_filename(const std::string& value) {
    const std::string lower = to_lower(value);
    // RFC 5987 form first: filename*=UTF-8''encoded
    const size_t star = lower.find("filename*=");
    if (star != std::string::npos) {
        std::string v = value.substr(star + 10);
        const size_t semi = v.find(';');
        if (semi != std::string::npos) v = v.substr(0, semi);
        v = trim(v);
        const size_t quote = v.find("''");
        if (quote != std::string::npos) v = v.substr(quote + 2);
        else {
            const size_t first_quote = v.find('\'');
            if (first_quote != std::string::npos) {
                const size_t second = v.find('\'', first_quote + 1);
                if (second != std::string::npos) v = v.substr(second + 1);
            }
        }
        return sanitize_filename(percent_decode(v));
    }
    const size_t plain = lower.find("filename=");
    if (plain != std::string::npos) {
        std::string v = value.substr(plain + 9);
        const size_t semi = v.find(';');
        if (semi != std::string::npos) v = v.substr(0, semi);
        v = trim(v);
        if (v.size() >= 2 && v.front() == '"' && v.back() == '"') {
            v = v.substr(1, v.size() - 2);
        }
        return sanitize_filename(v);
    }
    return "";
}

// ---------------------------------------------------------------------------
// Probe
// ---------------------------------------------------------------------------

struct ProbeCapture {
    long status = 0;
    bool got_status = false;
    uint64_t content_length = 0;
    uint64_t content_range_total = 0;
    bool accept_ranges = false;
    std::string etag;
    std::string last_modified;
    std::string content_type;
    std::string filename_hint;
    std::string error_body;  // start of a 4xx/5xx body, for the error message
};

size_t probe_header_cb(char* buffer, size_t size, size_t nitems, void* userdata) {
    auto* cap = static_cast<ProbeCapture*>(userdata);
    const size_t len = size * nitems;
    std::string line(buffer, len);
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
    if (line.empty()) return len;
    if (line.rfind("HTTP/", 0) == 0) {
        const size_t sp = line.find(' ');
        if (sp != std::string::npos) {
            cap->status = std::strtol(line.c_str() + sp + 1, nullptr, 10);
            cap->got_status = true;
        }
        return len;
    }
    const size_t colon = line.find(':');
    if (colon == std::string::npos) return len;
    const std::string name = to_lower(trim(line.substr(0, colon)));
    const std::string value = trim(line.substr(colon + 1));
    if (name == "content-length") {
        cap->content_length = std::strtoull(value.c_str(), nullptr, 10);
    } else if (name == "content-range") {
        const size_t slash = value.find('/');
        if (slash != std::string::npos) {
            const std::string total = trim(value.substr(slash + 1));
            if (total != "*") cap->content_range_total = std::strtoull(total.c_str(), nullptr, 10);
        }
    } else if (name == "accept-ranges") {
        cap->accept_ranges = to_lower(value) == "bytes";
    } else if (name == "etag") {
        cap->etag = value;
    } else if (name == "last-modified") {
        cap->last_modified = value;
    } else if (name == "content-type") {
        cap->content_type = value;
    } else if (name == "content-disposition") {
        const std::string fn = parse_content_disposition_filename(value);
        if (!fn.empty()) cap->filename_hint = fn;
    }
    return len;
}

// Successful probes abort as soon as the body starts (the headers are already
// captured); error responses keep their first bytes so the failure can explain
// itself.
size_t probe_body_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* cap = static_cast<ProbeCapture*>(userdata);
    const size_t total = size * nmemb;
    if (cap->status < 400) return 0;
    if (cap->error_body.size() >= kErrorBodyLimit) return 0;
    const size_t room = kErrorBodyLimit - cap->error_body.size();
    cap->error_body.append(ptr, std::min(total, room));
    return total;
}

// Runs one probe request. Returns true when a usable response arrived.
bool probe_once(const HttpOptions& o, curl_slist* headers, bool use_head,
                bool use_post, ProbeCapture& cap, std::string& effective_url,
                std::string& error) {
    CURL* handle = curl_easy_init();
    if (!handle) {
        error = "cannot initialize curl";
        return false;
    }
    char errbuf[CURL_ERROR_SIZE] = {0};
    apply_common_options(handle, o, headers);
    curl_easy_setopt(handle, CURLOPT_ERRORBUFFER, errbuf);
    curl_easy_setopt(handle, CURLOPT_HEADERFUNCTION, probe_header_cb);
    curl_easy_setopt(handle, CURLOPT_HEADERDATA, &cap);
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, probe_body_cb);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, &cap);
    if (use_post) {
        curl_easy_setopt(handle, CURLOPT_POST, 1L);
        curl_easy_setopt(handle, CURLOPT_POSTFIELDS, o.post_data.c_str());
        curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE,
                         static_cast<long>(o.post_data.size()));
    } else if (use_head) {
        curl_easy_setopt(handle, CURLOPT_NOBODY, 1L);
    } else {
        curl_easy_setopt(handle, CURLOPT_RANGE, "0-0");
    }

    const CURLcode rc = curl_easy_perform(handle);

    char* effective_ptr = nullptr;
    if (curl_easy_getinfo(handle, CURLINFO_EFFECTIVE_URL, &effective_ptr) == CURLE_OK &&
        effective_ptr) {
        effective_url = effective_ptr;
    }
    curl_easy_cleanup(handle);

    if (!cap.got_status) {
        if (rc == CURLE_OK) error = "no HTTP response";
        else error = curl_url_error(rc);
        return false;
    }
    // Writing is aborted intentionally once the body starts.
    if (rc != CURLE_OK && rc != CURLE_WRITE_ERROR) {
        error = curl_url_error(rc);
        return false;
    }
    error.clear();
    return true;
}

bool starts_with_ci(const std::string& text, const char* prefix) {
    const size_t n = std::strlen(prefix);
    if (text.size() < n) return false;
    for (size_t i = 0; i < n; ++i) {
        if (std::tolower(static_cast<unsigned char>(text[i])) != prefix[i]) return false;
    }
    return true;
}

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

// FTP's HEAD-only mode delivers synthetic "Content-Length"/"Accept-ranges"
// lines to the write callback, so the probe must consume them instead of
// aborting like the HTTP header probe does.
size_t discard_all_cb(char*, size_t size, size_t nmemb, void*) { return size * nmemb; }

// FTP probe: libcurl maps CURLOPT_NOBODY to a SIZE command, so the existing
// discard callback can be reused. FTP has no HTTP status; a transfer error is
// reported through the curl error buffer instead.
ProbeResult probe_ftp(const HttpOptions& options) {
    ProbeResult out;
    ensure_curl_global();
    CURL* handle = curl_easy_init();
    if (!handle) {
        out.error = "cannot initialize curl";
        return out;
    }
    char errbuf[CURL_ERROR_SIZE] = {0};
    curl_slist* headers = build_headers(options.headers);
    apply_common_options(handle, options, headers);
    curl_easy_setopt(handle, CURLOPT_ERRORBUFFER, errbuf);
    curl_easy_setopt(handle, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, discard_all_cb);
    const CURLcode rc = curl_easy_perform(handle);

    char* effective = nullptr;
    if (curl_easy_getinfo(handle, CURLINFO_EFFECTIVE_URL, &effective) == CURLE_OK && effective) {
        out.effective_url = effective;
    }
    curl_off_t length = -1;
    curl_easy_getinfo(handle, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &length);
    curl_easy_cleanup(handle);
    if (headers) curl_slist_free_all(headers);

    if (out.effective_url.empty()) out.effective_url = options.url;
    if (rc != CURLE_OK) {
        out.error = rc == CURLE_URL_MALFORMAT
                        ? kInvalidUrlError
                        : (errbuf[0] ? errbuf : curl_easy_strerror(rc));
        return out;
    }
    out.status_code = 0;  // no HTTP status over FTP
    out.content_length = length > 0 ? static_cast<uint64_t>(length) : 0;
    out.accept_ranges = out.content_length > 0;  // REST-based resume
    out.ok = true;
    return out;
}

struct TextCtx {
    std::string* text = nullptr;
    uint64_t max_bytes = 0;
    bool overflow = false;
};

size_t text_write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* ctx = static_cast<TextCtx*>(userdata);
    const size_t total = size * nmemb;
    if (total == 0) return 0;
    if (ctx->text->size() + total > ctx->max_bytes) {
        ctx->overflow = true;
        return 0;
    }
    ctx->text->append(ptr, total);
    return total;
}

}  // namespace

bool is_ftp_url(const std::string& url) {
    return starts_with_ci(url, "ftp://") || starts_with_ci(url, "ftps://");
}

bool fetch_text(const HttpOptions& options, std::string* text, std::string* effective_url,
                std::string* error, uint64_t max_bytes) {
    if (text) text->clear();
    if (!is_supported_download_url(options.url)) {
        if (error) *error = kInvalidUrlError;
        return false;
    }
    ensure_curl_global();
    CURL* handle = curl_easy_init();
    if (!handle) {
        if (error) *error = "cannot initialize curl";
        return false;
    }
    curl_slist* headers = build_headers(options.headers);
    char errbuf[CURL_ERROR_SIZE] = {0};
    TextCtx ctx{text, max_bytes};
    apply_common_options(handle, options, headers);
    curl_easy_setopt(handle, CURLOPT_ERRORBUFFER, errbuf);
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, text_write_cb);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, &ctx);
    const CURLcode rc = curl_easy_perform(handle);
    long code = 0;
    curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &code);
    char* effective = nullptr;
    if (curl_easy_getinfo(handle, CURLINFO_EFFECTIVE_URL, &effective) == CURLE_OK && effective &&
        effective_url) {
        *effective_url = effective;
    }
    curl_easy_cleanup(handle);
    if (headers) curl_slist_free_all(headers);

    if (effective_url && effective_url->empty()) *effective_url = options.url;
    if (rc != CURLE_OK) {
        if (error) {
            if (ctx.overflow) *error = "response is larger than the allowed size";
            else if (rc == CURLE_URL_MALFORMAT) *error = kInvalidUrlError;
            else *error = errbuf[0] ? errbuf : curl_easy_strerror(rc);
        }
        return false;
    }
    if (code >= 400) {
        if (error) *error = http_error_text(code);
        return false;
    }
    if (error) error->clear();
    return true;
}

namespace {

// Probe without interstitial handling: HEAD first, GET Range: 0-0 as fallback.
ProbeResult probe_url_once(const HttpOptions& options) {
    if (is_ftp_url(options.url)) return probe_ftp(options);
    ensure_curl_global();
    ProbeResult out;
    curl_slist* headers = build_headers(options.headers);

    ProbeCapture cap;
    std::string error;
    std::string effective_url = options.url;

    // 1) HEAD (unless POST data requires a POST probe).
    if (options.post_data.empty()) {
        if (probe_once(options, headers, /*use_head=*/true, /*use_post=*/false, cap,
                       effective_url, error)) {
            if (cap.status >= 200 && cap.status < 400) {
                // usable
            } else {
                cap = ProbeCapture{};  // fall through to GET range probe
            }
        }
    }

    // 2) GET with Range: 0-0 (or POST).
    if (!cap.got_status) {
        const bool use_post = !options.post_data.empty();
        if (!probe_once(options, headers, /*use_head=*/false, use_post, cap, effective_url,
                        error)) {
            if (headers) curl_slist_free_all(headers);
            out.error = error.empty() ? "probe failed" : error;
            return out;
        }
    }

    if (headers) curl_slist_free_all(headers);

    out.status_code = cap.status;
    out.effective_url = effective_url.empty() ? options.url : effective_url;
    out.accept_ranges = cap.accept_ranges;
    out.filename_hint = cap.filename_hint;
    out.mime_type = cap.content_type;
    const size_t semi = out.mime_type.find(';');
    if (semi != std::string::npos) out.mime_type = trim(out.mime_type.substr(0, semi));
    out.etag = cap.etag;
    out.last_modified = cap.last_modified;

    if (cap.status >= 400 && cap.status != 416) {
        out.error = http_error_with_detail(cap.status, cap.error_body, cap.content_type);
        return out;
    }
    out.content_length = cap.content_range_total;
    if (out.content_length == 0) out.content_length = cap.content_length;
    out.ok = true;
    return out;
}

// Bodies read while resolving an interstitial. Drive's virus-scan notice is a
// few KB; the cap keeps a mistaken fetch of a real HTML file short.
constexpr uint64_t kInterstitialBodyLimit = 256 * 1024;

// A file response carries Content-Disposition and/or range support; an HTML
// body without either is one of the site's pages.
bool is_web_page(const ProbeResult& probe) {
    return probe.ok && probe.mime_type == "text/html" && probe.filename_hint.empty() &&
           !probe.accept_ranges;
}

// scheme://host:port with the scheme's default port filled in, so two URLs
// compare equal regardless of how the port is written. Empty for a URL without
// a scheme or authority.
std::string url_origin(const std::string& url) {
    const size_t scheme_end = url.find("://");
    if (scheme_end == std::string::npos) return {};
    const std::string scheme = to_lower(url.substr(0, scheme_end));
    const size_t authority_start = scheme_end + 3;
    const size_t authority_end = url.find_first_of("/?#", authority_start);
    std::string authority = url.substr(
        authority_start, authority_end == std::string::npos ? std::string::npos
                                                            : authority_end - authority_start);
    const size_t userinfo = authority.rfind('@');
    if (userinfo != std::string::npos) authority.erase(0, userinfo + 1);
    if (authority.empty()) return {};

    std::string host = authority;
    std::string port;
    if (host.front() == '[') {
        const size_t bracket = host.find(']');
        if (bracket == std::string::npos) return {};
        if (bracket + 1 < host.size() && host[bracket + 1] == ':') {
            port = host.substr(bracket + 2);
        }
        host.resize(bracket + 1);
    } else if (const size_t colon = host.find(':'); colon != std::string::npos) {
        port = host.substr(colon + 1);
        host.resize(colon);
    }
    if (host.empty()) return {};
    if (port.empty()) {
        if (scheme == "https" || scheme == "ftps") {
            port = "443";
        } else if (scheme == "ftp") {
            port = "21";
        } else {
            port = "80";
        }
    }
    return scheme + "://" + to_lower(host) + ":" + port;
}

// Resolves a page that answered instead of the file (04-protocols.md §5):
// download-confirmation notices are submitted transparently, everything else
// is reported instead of being stored as the "file".
ProbeResult resolve_interstitial(const HttpOptions& options, ProbeResult probe) {
    if (const std::string reason = signin_error_reason(probe.effective_url, {}); !reason.empty()) {
        probe.ok = false;
        probe.error = reason;
        return probe;
    }

    std::string body;
    std::string body_url;
    std::string fetch_error;
    if (!fetch_text(options, &body, &body_url, &fetch_error, kInterstitialBodyLimit)) {
        const std::string reason = page_error_reason(options.url, probe.effective_url);
        if (!reason.empty()) {
            probe.ok = false;
            probe.error = reason + " (" +
                          (fetch_error.empty() ? std::string("page unreadable") : fetch_error) + ")";
        }
        return probe;
    }
    if (const std::string reason = signin_error_reason(body_url, body); !reason.empty()) {
        probe.ok = false;
        probe.error = reason;
        return probe;
    }

    const std::string confirm = download_confirmation_url(body);
    if (!confirm.empty()) {
        HttpOptions next = options;
        // The notice does not repeat the link's account/resource-key handoff.
        next.url = keep_drive_handoff_params(confirm, options.url);
        // The notice may only steer the second request to the origin that
        // served it. Without this, any page could aim the follow-up request -
        // and the source of the saved file - at an address of its choosing,
        // including hosts on the local network (audit finding 2). Drive's
        // handoff between its own download hosts stays allowed.
        const std::string page_url = body_url.empty() ? options.url : body_url;
        const std::string page_origin = url_origin(page_url);
        const bool same_origin =
            !page_origin.empty() && page_origin == url_origin(next.url);
        const bool drive_handoff =
            is_google_drive_host(page_url) && is_google_drive_host(next.url);
        if (!same_origin && !drive_handoff) {
            probe.ok = false;
            probe.error = kConfirmationPendingError;
            return probe;
        }
        if (!is_supported_download_url(next.url)) {
            probe.ok = false;
            probe.error = kConfirmationPendingError;
            return probe;
        }
        ProbeResult resolved = probe_url_once(next);  // exactly one confirmation step
        if (resolved.ok && is_web_page(resolved)) {
            resolved.ok = false;
            resolved.error = kConfirmationPendingError;
        }
        if (!resolved.ok) {
            probe.ok = false;
            probe.error = resolved.error.empty() ? std::string(kConfirmationPendingError)
                                                : resolved.error;
            return probe;
        }
        resolved.download_url = next.url;
        return resolved;
    }

    const std::string reason = page_error_reason(options.url, body_url);
    if (!reason.empty()) {
        std::string detail = condense_error_body(body, probe.mime_type);
        if (detail.empty()) detail = condense_error_body(body, "text/html");
        probe.ok = false;
        probe.error = reason;
        if (!detail.empty()) probe.error += " - " + detail;
    }
    return probe;
}

}  // namespace

ProbeResult probe_url(const HttpOptions& options) {
    if (!is_supported_download_url(options.url)) {
        ProbeResult out;
        out.error = kInvalidUrlError;
        return out;
    }

    HttpOptions target = options;
    target.url = encode_url_for_request(options.url);

    // Drive folder links have no file behind them; fail before any request so
    // the reason reaches the user instead of a saved page named after the id.
    if (!google_drive_folder_id(target.url).empty()) {
        ProbeResult out;
        out.error = kDriveFolderError;
        return out;
    }

    const std::string file_id = google_drive_file_id(target.url);
    if (!file_id.empty()) {
        // Viewer links ("/file/d/<id>/view", "/open?id=", "/drive/u/<n>/…")
        // serve the Drive web app; the direct URL is what the site itself
        // redirects to for "uc?export=download" links.
        target.url =
            keep_drive_handoff_params(google_drive_download_url(file_id), target.url);
    }

    ProbeResult out = probe_url_once(target);
    // POST downloads are API calls, not site links; leave their body alone.
    if (options.post_data.empty() && is_web_page(out)) {
        out = resolve_interstitial(target, std::move(out));
    }
    if (out.download_url.empty() && target.url != options.url) out.download_url = target.url;
    return out;
}

// ---------------------------------------------------------------------------
// SegmentDownloader
// ---------------------------------------------------------------------------

struct SegmentDownloader::Impl {
    struct HandleCtx {
        Impl* impl = nullptr;
        size_t index = 0;
        long status = 0;
        bool range_bad = false;
        bool interrupted = false;
        std::string content_type;
        std::string error_body;  // start of a 4xx/5xx body, for the error message
    };

    HttpOptions options;
    TransferState& state;
    int fd;

    CURLM* multi = nullptr;
    curl_slist* headers = nullptr;

    std::shared_ptr<BandwidthLimit> bandwidth_limit;
    bool one_by_one = false;
    int max_retries = kMaxRetries;

    struct SegRuntime {
        int attempts = 0;
        int64_t next_try_ms = 0;
        bool active = false;
        bool done = false;
        CURL* easy = nullptr;
        std::string error_buffer;
    };
    std::vector<SegRuntime> runtime;
    std::vector<HandleCtx> contexts;

    explicit Impl(const HttpOptions& o, TransferState& s, int fd_)
        : options(o), state(s), fd(fd_) {}

    uint64_t done_bytes() const { return state.done_bytes(); }

    static size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
        auto* ctx = static_cast<HandleCtx*>(userdata);
        Impl* impl = ctx->impl;
        const size_t total = size * nmemb;
        if (total == 0 || ctx->range_bad) return 0;
        // Redirect and error bodies must not land in the output file; keep the
        // first bytes of a 4xx/5xx body so the failure can explain itself.
        if (ctx->status >= 300) {
            if (ctx->status >= 400 && ctx->error_body.size() < kErrorBodyLimit) {
                const size_t room = kErrorBodyLimit - ctx->error_body.size();
                ctx->error_body.append(ptr, std::min(total, room));
            }
            return total;
        }
        if (impl->bandwidth_limit && !impl->bandwidth_limit->wait_for_bytes(total)) {
            ctx->interrupted = true;
            return 0;
        }
        Segment& seg = impl->state.segments[ctx->index];
        if (seg.done >= seg.length()) return total;  // nothing left; discard
        uint64_t to_write = total;
        const uint64_t remaining = seg.length() - seg.done;
        if (to_write > remaining) to_write = remaining;
        const uint64_t offset = seg.start + seg.done;
        const ssize_t written = ::pwrite(impl->fd, ptr, to_write, static_cast<off_t>(offset));
        if (written < 0 || static_cast<uint64_t>(written) != to_write) return 0;
        seg.done += to_write;
        return total;
    }

    static size_t header_cb(char* buffer, size_t size, size_t nitems, void* userdata) {
        auto* ctx = static_cast<HandleCtx*>(userdata);
        const size_t len = size * nitems;
        const std::string line(buffer, len);
        if (line.rfind("HTTP/", 0) == 0) {
            const size_t sp = line.find(' ');
            if (sp != std::string::npos) {
                ctx->status = std::strtol(line.c_str() + sp + 1, nullptr, 10);
                // A redirect starts a new response: drop the previous captions.
                ctx->content_type.clear();
                ctx->error_body.clear();
                const Segment& seg = ctx->impl->state.segments[ctx->index];
                const uint64_t from = seg.start + seg.done;
                if (ctx->status == 200 && from > 0) ctx->range_bad = true;
            }
            return len;
        }
        if (starts_with_ci(line, "content-type")) {
            const size_t colon = line.find(':');
            if (colon != std::string::npos) ctx->content_type = trim(line.substr(colon + 1));
        }
        return len;
    }

    bool start_segment(size_t i) {
        Segment& seg = state.segments[i];
        if (seg.finished()) {
            runtime[i].done = true;
            return false;
        }
        CURL* handle = curl_easy_init();
        if (!handle) return false;
        HandleCtx& ctx = contexts[i];
        ctx.impl = this;
        ctx.index = i;
        ctx.status = 0;
        ctx.range_bad = false;
        ctx.interrupted = false;
        ctx.content_type.clear();
        ctx.error_body.clear();

        runtime[i].error_buffer.assign(CURL_ERROR_SIZE, '\0');
        apply_common_options(handle, options, headers);
        curl_easy_setopt(handle, CURLOPT_ERRORBUFFER, runtime[i].error_buffer.data());
        curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, write_cb);
        curl_easy_setopt(handle, CURLOPT_WRITEDATA, &ctx);
        curl_easy_setopt(handle, CURLOPT_HEADERFUNCTION, header_cb);
        curl_easy_setopt(handle, CURLOPT_HEADERDATA, &ctx);
        const std::string range =
            std::to_string(seg.start + seg.done) + "-" + std::to_string(seg.end);
        curl_easy_setopt(handle, CURLOPT_RANGE, range.c_str());

        if (curl_multi_add_handle(multi, handle) != CURLM_OK) {
            curl_easy_cleanup(handle);
            return false;
        }
        runtime[i].easy = handle;
        runtime[i].active = true;
        ++runtime[i].attempts;
        return true;
    }

    // Returns: 0 = retry scheduled, 1 = terminal error, 2 = interrupted.
    int finalize_segment(size_t i, CURLcode rc, std::string& error) {
        SegRuntime& rt = runtime[i];
        CURL* handle = rt.easy;
        rt.easy = nullptr;
        rt.active = false;
        long code = 0;
        curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &code);
        curl_multi_remove_handle(multi, handle);
        curl_easy_cleanup(handle);

        Segment& seg = state.segments[i];
        if (contexts[i].interrupted) {
            error = "transfer interrupted";
            return 2;
        }
        if (rc == CURLE_OK && code >= 200 && code < 300 && seg.finished()) {
            rt.done = true;
            return 0;
        }
        if (contexts[i].range_bad) {
            error = "server does not support range requests";
            return 1;
        }
        if (rc == CURLE_WRITE_ERROR && !contexts[i].range_bad) {
            error = "disk write failed";
            return 1;
        }
        bool retryable = false;
        if (code == 408 || code == 429 || code >= 500) {
            retryable = true;
        } else if (code >= 400 && code != 416) {
            retryable = false;
        } else if (code == 416) {
            retryable = false;
        } else {
            switch (rc) {
                case CURLE_COULDNT_RESOLVE_HOST:
                case CURLE_COULDNT_CONNECT:
                case CURLE_OPERATION_TIMEDOUT:
                case CURLE_RECV_ERROR:
                case CURLE_SEND_ERROR:
                case CURLE_GOT_NOTHING:
                case CURLE_PARTIAL_FILE:
                case CURLE_SSL_CONNECT_ERROR:
                case CURLE_HTTP2_STREAM:
                    retryable = true;
                    break;
                default:
                    retryable = false;
            }
        }
        if (!retryable || rt.attempts > max_retries) {
            if (code > 0) {
                error = http_error_with_detail(code, contexts[i].error_body,
                                               contexts[i].content_type);
            } else if (rc == CURLE_URL_MALFORMAT) {
                error = kInvalidUrlError;
            } else if (!rt.error_buffer.empty() && rt.error_buffer[0] != '\0') {
                error = rt.error_buffer.c_str();
            } else {
                error = curl_url_error(rc);
            }
            return 1;
        }
        const int64_t backoff = std::min<int64_t>(16000, 1000LL << (rt.attempts - 1));
        rt.next_try_ms = now_ms() + backoff;
        log_warn("segment " + std::to_string(i) + " retry " +
                 std::to_string(rt.attempts) + "/" + std::to_string(max_retries) +
                 " in " + std::to_string(backoff) + " ms");
        return 0;
    }

    void cleanup_handles() {
        for (size_t i = 0; i < runtime.size(); ++i) {
            if (runtime[i].easy) {
                curl_multi_remove_handle(multi, runtime[i].easy);
                curl_easy_cleanup(runtime[i].easy);
                runtime[i].easy = nullptr;
                runtime[i].active = false;
            }
        }
    }
};

SegmentDownloader::SegmentDownloader(const HttpOptions& options, TransferState& state,
                                     int write_fd)
    : impl_(std::make_unique<Impl>(options, state, write_fd)) {}

SegmentDownloader::~SegmentDownloader() = default;

void SegmentDownloader::set_bandwidth_limit(std::shared_ptr<BandwidthLimit> bandwidth_limit) {
    impl_->bandwidth_limit = std::move(bandwidth_limit);
}

void SegmentDownloader::set_one_by_one(bool one_by_one) { impl_->one_by_one = one_by_one; }

void SegmentDownloader::set_max_retries(int retries) { impl_->max_retries = retries; }

TransferOutcome SegmentDownloader::run(const ProgressFn& on_progress, std::string* error) {
    ensure_curl_global();
    Impl& impl = *impl_;

    if (impl.state.finished()) {
        if (error) error->clear();
        return TransferOutcome::Completed;
    }

    impl.multi = curl_multi_init();
    if (!impl.multi) {
        if (error) *error = "cannot initialize curl multi";
        return TransferOutcome::Failed;
    }
    impl.headers = build_headers(impl.options.headers);
    const size_t count = impl.state.segments.size();
    impl.runtime.assign(count, Impl::SegRuntime{});
    impl.contexts.assign(count, Impl::HandleCtx{});

    TransferOutcome outcome = TransferOutcome::Failed;
    std::string failure;
    double speed = 0.0;
    uint64_t last_done = impl.done_bytes();
    int64_t last_sample = now_ms();
    int64_t last_report = now_ms();

    while (true) {
        const int64_t now = now_ms();

        // Start eligible segments.
        int active = 0;
        for (const auto& rt : impl.runtime) {
            if (rt.active) ++active;
        }
        for (size_t i = 0; i < count; ++i) {
            Impl::SegRuntime& rt = impl.runtime[i];
            if (rt.active || rt.done || impl.state.segments[i].finished()) {
                if (impl.state.segments[i].finished() && !rt.active) rt.done = true;
                continue;
            }
            if (rt.next_try_ms > now) continue;
            if (impl.one_by_one && active >= 1) break;
            if (impl.start_segment(i)) ++active;
            else {
                failure = "cannot start transfer";
                outcome = TransferOutcome::Failed;
                goto finish;
            }
        }
        // Finished everything?
        if (impl.state.finished()) {
            outcome = TransferOutcome::Completed;
            goto finish;
        }

        // No active handles: either waiting for a retry or dead.
        if (active == 0 && impl.multi) {
            bool waiting = false;
            int64_t soonest = now + 1000;
            for (const auto& rt : impl.runtime) {
                if (!rt.done && rt.next_try_ms > now) {
                    waiting = true;
                    soonest = std::min(soonest, rt.next_try_ms);
                }
            }
            if (!waiting) {
                // Nothing left to do and not finished -> terminal failure.
                failure = failure.empty() ? "transfer stalled" : failure;
                outcome = TransferOutcome::Failed;
                goto finish;
            }
            const int64_t sleep_ms = std::max<int64_t>(10, std::min<int64_t>(100, soonest - now));
            curl_multi_poll(impl.multi, nullptr, 0, static_cast<int>(sleep_ms), nullptr);
        } else {
            int still_running = 0;
            curl_multi_perform(impl.multi, &still_running);
            CURLMsg* msg = nullptr;
            int pending = 0;
            while ((msg = curl_multi_info_read(impl.multi, &pending)) != nullptr) {
                if (msg->msg != CURLMSG_DONE) continue;
                size_t idx = count;
                for (size_t i = 0; i < count; ++i) {
                    if (impl.runtime[i].easy == msg->easy_handle) {
                        idx = i;
                        break;
                    }
                }
                if (idx == count) continue;
                std::string seg_error;
                const int finalized = impl.finalize_segment(idx, msg->data.result, seg_error);
                if (finalized == 2) {
                    outcome = TransferOutcome::Paused;
                    goto finish;
                }
                if (finalized == 1) {
                    failure = seg_error;
                    outcome = TransferOutcome::Failed;
                    goto finish;
                }
            }
            curl_multi_poll(impl.multi, nullptr, 0, kIoTimeoutMs, nullptr);
        }

        // Speed sampling and progress/pause reporting.
        const int64_t t = now_ms();
        const uint64_t done = impl.done_bytes();
        if (t - last_sample >= 500) {
            const double dt = static_cast<double>(t - last_sample) / 1000.0;
            if (dt > 0) {
                const double inst = static_cast<double>(done - last_done) / dt;
                speed = (speed == 0.0) ? inst : (0.6 * speed + 0.4 * inst);
            }
            last_done = done;
            last_sample = t;
        }
        if (on_progress && t - last_report >= kReportIntervalMs) {
            last_report = t;
            if (!on_progress(done, impl.state.file_size, speed)) {
                outcome = TransferOutcome::Paused;
                goto finish;
            }
        }
    }

finish:
    impl.cleanup_handles();
    if (impl.headers) {
        curl_slist_free_all(impl.headers);
        impl.headers = nullptr;
    }
    if (impl.multi) {
        curl_multi_cleanup(impl.multi);
        impl.multi = nullptr;
    }
    if (error) *error = failure;
    return outcome;
}

// ---------------------------------------------------------------------------
// stream_download (unknown size / no range support)
// ---------------------------------------------------------------------------

namespace {

struct StreamCtx {
    int fd = -1;
    uint64_t written = 0;
    uint64_t base = 0;  // bytes already on disk before this transfer
    const std::function<bool(uint64_t)>* on_progress = nullptr;
    int64_t last_report = 0;
    long status = 0;
    std::string content_type;
    std::string error_body;  // start of a 4xx/5xx body, for the error message
    std::shared_ptr<BandwidthLimit> bandwidth_limit;
    bool interrupted = false;
};

size_t stream_write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* ctx = static_cast<StreamCtx*>(userdata);
    const size_t total = size * nmemb;
    if (total == 0) return 0;
    // Redirect and error bodies must not land in the output file.
    if (ctx->status >= 300) {
        if (ctx->status >= 400 && ctx->error_body.size() < kErrorBodyLimit) {
            const size_t room = kErrorBodyLimit - ctx->error_body.size();
            ctx->error_body.append(ptr, std::min(total, room));
        }
        return total;
    }
    if (ctx->bandwidth_limit && !ctx->bandwidth_limit->wait_for_bytes(total)) {
        ctx->interrupted = true;
        return 0;
    }
    const ssize_t written = ::write(ctx->fd, ptr, total);
    if (written < 0 || static_cast<size_t>(written) != total) return 0;
    ctx->written += total;
    return total;
}

size_t stream_header_cb(char* buffer, size_t size, size_t nitems, void* userdata) {
    auto* ctx = static_cast<StreamCtx*>(userdata);
    const size_t len = size * nitems;
    const std::string line(buffer, len);
    if (line.rfind("HTTP/", 0) == 0) {
        const size_t sp = line.find(' ');
        if (sp != std::string::npos) {
            ctx->status = std::strtol(line.c_str() + sp + 1, nullptr, 10);
            ctx->content_type.clear();
            ctx->error_body.clear();
        }
        return len;
    }
    if (starts_with_ci(line, "content-type")) {
        const size_t colon = line.find(':');
        if (colon != std::string::npos) ctx->content_type = trim(line.substr(colon + 1));
    }
    return len;
}

int stream_progress_cb(void* clientp, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    auto* ctx = static_cast<StreamCtx*>(clientp);
    const int64_t now = now_ms();
    if (now - ctx->last_report >= kReportIntervalMs) {
        ctx->last_report = now;
        if (ctx->on_progress && !(*ctx->on_progress)(ctx->written + ctx->base)) return 1;  // pause
    }
    return 0;
}

}  // namespace

TransferOutcome stream_download(const HttpOptions& options, int write_fd,
                                const std::function<bool(uint64_t done)>& on_progress,
                                uint64_t* bytes_written, std::string* error,
                                uint64_t resume_from,
                                std::shared_ptr<BandwidthLimit> bandwidth_limit) {
    ensure_curl_global();
    if (!is_supported_download_url(options.url)) {
        if (error) *error = kInvalidUrlError;
        return TransferOutcome::Failed;
    }
    curl_slist* headers = build_headers(options.headers);
    CURL* handle = curl_easy_init();
    if (!handle) {
        if (headers) curl_slist_free_all(headers);
        if (error) *error = "cannot initialize curl";
        return TransferOutcome::Failed;
    }

    StreamCtx ctx;
    ctx.fd = write_fd;
    ctx.base = resume_from;
    ctx.on_progress = &on_progress;
    ctx.last_report = now_ms();
    ctx.bandwidth_limit = std::move(bandwidth_limit);

    char errbuf[CURL_ERROR_SIZE] = {0};
    apply_common_options(handle, options, headers);
    curl_easy_setopt(handle, CURLOPT_ERRORBUFFER, errbuf);
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, stream_write_cb);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(handle, CURLOPT_HEADERFUNCTION, stream_header_cb);
    curl_easy_setopt(handle, CURLOPT_HEADERDATA, &ctx);
    curl_easy_setopt(handle, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(handle, CURLOPT_XFERINFOFUNCTION, stream_progress_cb);
    curl_easy_setopt(handle, CURLOPT_XFERINFODATA, &ctx);
    if (resume_from > 0 && options.post_data.empty()) {
        if (::lseek(write_fd, static_cast<off_t>(resume_from), SEEK_SET) < 0) {
            curl_easy_cleanup(handle);
            if (headers) curl_slist_free_all(headers);
            if (error) *error = "cannot seek in the partial file";
            return TransferOutcome::Failed;
        }
        curl_easy_setopt(handle, CURLOPT_RESUME_FROM_LARGE,
                         static_cast<curl_off_t>(resume_from));
    }
    if (!options.post_data.empty()) {
        curl_easy_setopt(handle, CURLOPT_POST, 1L);
        curl_easy_setopt(handle, CURLOPT_POSTFIELDS, options.post_data.c_str());
        curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE,
                         static_cast<long>(options.post_data.size()));
    }

    const CURLcode rc = curl_easy_perform(handle);
    long code = 0;
    curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(handle);
    if (headers) curl_slist_free_all(headers);
    if (bytes_written) *bytes_written = ctx.written + ctx.base;

    if (rc == CURLE_ABORTED_BY_CALLBACK || ctx.interrupted) return TransferOutcome::Paused;
    if (rc != CURLE_OK) {
        if (error) {
            *error = rc == CURLE_URL_MALFORMAT
                         ? kInvalidUrlError
                         : (errbuf[0] ? errbuf : curl_easy_strerror(rc));
        }
        return TransferOutcome::Failed;
    }
    if (code >= 400) {
        if (error) *error = http_error_with_detail(code, ctx.error_body, ctx.content_type);
        return TransferOutcome::Failed;
    }
    if (error) error->clear();
    return TransferOutcome::Completed;
}

}  // namespace swiftdm
