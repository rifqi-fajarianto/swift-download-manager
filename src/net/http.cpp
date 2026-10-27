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

namespace swiftdm {

using namespace detail;

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
    // Credentials in the URL are refused (the browser integration already does
    // so on its side): the userinfo would travel in clear text through the
    // logs, the history and the request line. Proxy authentication has its own
    // setting.
    if (userinfo_end != std::string::npos) return false;
    if (authority.empty() || authority.find_first_of(" \t\r\n") != std::string::npos) return false;

    if (authority.front() == '[') {
        const size_t bracket = authority.find(']');
        return bracket != std::string::npos && bracket > 1;
    }
    const size_t colon = authority.find(':');
    if (colon != std::string::npos) authority.resize(colon);
    return !authority.empty();
}

bool same_url_origin(const std::string& left, const std::string& right) {
    const std::string a = request_origin(left.c_str());
    return !a.empty() && a == request_origin(right.c_str());
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
    HttpOptions current = options;
    for (int attempt = 0; attempt <= kMaxRedirects; ++attempt) {
        if (text) text->clear();
        CURL* handle = curl_easy_init();
        if (!handle) {
            if (error) *error = "cannot initialize curl";
            return false;
        }
        curl_slist* headers = build_headers(current.headers);
        char errbuf[CURL_ERROR_SIZE] = {0};
        TextCtx ctx{text, max_bytes};
        RequestOriginGuard origin_guard;
        apply_common_options(handle, current, headers, &origin_guard);
        curl_easy_setopt(handle, CURLOPT_ERRORBUFFER, errbuf);
        curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, text_write_cb);
        curl_easy_setopt(handle, CURLOPT_WRITEDATA, &ctx);
        const CURLcode rc = curl_easy_perform(handle);
        long code = 0;
        curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &code);
        char* effective = nullptr;
        if (curl_easy_getinfo(handle, CURLINFO_EFFECTIVE_URL, &effective) == CURLE_OK &&
            effective && effective_url) {
            *effective_url = effective;
        }
        curl_easy_cleanup(handle);
        if (headers) curl_slist_free_all(headers);

#if LIBCURL_VERSION_NUM < 0x075000
        if (origin_guard.guarded && code >= 300 && code < 400) {
            origin_guard.crossed_origin = true;
        }
#endif
        if (origin_guard.crossed_origin) {
            if (origin_guard.blocked_url.empty()) {
                if (error) *error = kRedirectWithSensitiveHeadersError;
                return false;
            }
            if (!is_supported_download_url(origin_guard.blocked_url)) {
                if (error) *error = kInvalidUrlError;
                return false;
            }
            current.url = origin_guard.blocked_url;
            current.headers.clear();
            current.referer.clear();
            continue;
        }
        if (effective_url && effective_url->empty()) *effective_url = current.url;
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
        if (origin_guard.guarded && code >= 300 && code < 400) {
            if (error) *error = kRedirectWithSensitiveHeadersError;
            return false;
        }
        if (error) error->clear();
        return true;
    }
    if (error) *error = "too many redirects";
    return false;
}


}  // namespace swiftdm
