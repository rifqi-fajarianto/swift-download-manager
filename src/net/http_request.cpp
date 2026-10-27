#include "net/http.h"

#include <curl/curl.h>

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

curl_slist* build_headers(const std::vector<HttpHeader>& headers) {
    curl_slist* list = nullptr;
    for (const HttpHeader& h : headers) {
        if (h.name.empty()) continue;
        list = curl_slist_append(list, (h.name + ": " + h.value).c_str());
    }
    return list;
}

bool request_has_sensitive_headers(const HttpOptions& options) {
    return !options.headers.empty() || !options.referer.empty();
}

std::string request_origin(const char* url) {
    if (!url) return {};
    const std::string text(url);
    const size_t scheme_end = text.find("://");
    if (scheme_end == std::string::npos) return {};
    const std::string scheme = to_lower(text.substr(0, scheme_end));
    const size_t authority_start = scheme_end + 3;
    const size_t authority_end = text.find_first_of("/?#", authority_start);
    std::string authority = text.substr(
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
        if (scheme == "https" || scheme == "ftps") port = "443";
        else if (scheme == "ftp") port = "21";
        else port = "80";
    }
    return scheme + "://" + to_lower(host) + ":" + port;
}

int redirect_origin_guard(void* userdata, char*, char*, int, int) {
    auto* guard = static_cast<RequestOriginGuard*>(userdata);
    char* effective_url = nullptr;
    if (!guard->handle ||
        curl_easy_getinfo(guard->handle, CURLINFO_EFFECTIVE_URL, &effective_url) != CURLE_OK ||
        !effective_url) {
        return CURL_PREREQFUNC_ABORT;
    }
    if (request_origin(effective_url) != guard->original_origin) {
        guard->crossed_origin = true;
        return CURL_PREREQFUNC_ABORT;
    }
    return CURL_PREREQFUNC_OK;
}

void apply_common_options(CURL* handle, const HttpOptions& o, curl_slist* headers,
                          RequestOriginGuard* origin_guard = nullptr) {
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

    // MSYS2 builds of libcurl default to the POSIX path
    // /ucrt64/etc/ssl/certs/ca-bundle.crt, which does not exist on Windows;
    // without an explicit bundle every HTTPS handshake fails with
    // CURLE_SSL_CACERT_BADFILE (error 77). The Windows package ships
    // ca-bundle.crt next to the executable. On Linux that file is absent, so
    // the lookup returns nothing and libcurl keeps its built-in system
    // default. Resolved once: every request must present the same bundle.
    static const std::string ca_bundle = resolve_ca_bundle();
    if (!ca_bundle.empty()) curl_easy_setopt(handle, CURLOPT_CAINFO, ca_bundle.c_str());

    curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1L);
    if (origin_guard && request_has_sensitive_headers(o)) {
        origin_guard->guarded = true;
        origin_guard->original_origin = request_origin(request_url.c_str());
        origin_guard->handle = handle;
#if LIBCURL_VERSION_NUM >= 0x075000
        curl_easy_setopt(handle, CURLOPT_PREREQFUNCTION, redirect_origin_guard);
        curl_easy_setopt(handle, CURLOPT_PREREQDATA, origin_guard);
#else
        // Without a pre-request hook, fail closed on any redirect when custom
        // headers/referer are present rather than risk forwarding them.
        curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 0L);
#endif
    }
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

bool parse_content_range(const std::string& value, uint64_t* start, uint64_t* end,
                         uint64_t* total) {
    const std::string text = trim(value);
    constexpr std::string_view prefix = "bytes ";
    if (text.rfind(prefix, 0) != 0) return false;
    const size_t dash = text.find('-', prefix.size());
    const size_t slash = text.find('/', dash == std::string::npos ? prefix.size() : dash + 1);
    if (dash == std::string::npos || slash == std::string::npos || slash <= dash + 1 ||
        slash + 1 >= text.size()) {
        return false;
    }
    const auto parse_number = [](std::string_view token, uint64_t* output) {
        if (token.empty()) return false;
        const auto [end_ptr, ec] = std::from_chars(token.data(), token.data() + token.size(), *output);
        return ec == std::errc{} && end_ptr == token.data() + token.size();
    };
    return parse_number(std::string_view(text).substr(prefix.size(), dash - prefix.size()), start) &&
           parse_number(std::string_view(text).substr(dash + 1, slash - dash - 1), end) &&
           parse_number(std::string_view(text).substr(slash + 1), total) && *start <= *end &&
           *end < *total;
}

bool starts_with_ci(const std::string& text, const char* prefix) {
    const size_t n = std::strlen(prefix);
    if (text.size() < n) return false;
    for (size_t i = 0; i < n; ++i) {
        if (std::tolower(static_cast<unsigned char>(text[i])) != prefix[i]) return false;
    }
    return true;
}

}  // namespace swiftdm::detail
