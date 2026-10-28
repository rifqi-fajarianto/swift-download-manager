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
    std::string creation_date;
    std::string content_type;
    std::string filename_hint;
    std::string error_body;  // start of a 4xx/5xx body, for the error message
};

int month_index(const char* name) {
    static const char* const kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                          "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    for (int i = 0; i < 12; ++i) {
        if (std::strncmp(name, kMonths[i], 3) == 0) return i + 1;
    }
    return 0;
}

int64_t days_to_unix(int year, int month, int day, int hour, int minute, int second) {
    const std::chrono::year_month_day ymd{
        std::chrono::year{year}, std::chrono::month{static_cast<unsigned>(month)},
        std::chrono::day{static_cast<unsigned>(day)}};
    if (!ymd.ok() || hour > 23 || minute > 59 || second > 60) return 0;
    const auto tp = std::chrono::sys_days{ymd} + std::chrono::hours{hour} +
                    std::chrono::minutes{minute} + std::chrono::seconds{second};
    return std::chrono::duration_cast<std::chrono::seconds>(tp.time_since_epoch()).count();
}

// RFC 1123 date ("Wed, 21 Oct 2015 07:28:00 GMT"). 0 when it does not parse.
int64_t parse_http_date(const std::string& value) {
    char weekday[4] = {0};
    char month[4] = {0};
    int day = 0, year = 0, hour = 0, minute = 0, second = 0;
    if (std::sscanf(value.c_str(), "%3s, %d %3s %d %d:%d:%d", weekday, &day, month, &year,
                    &hour, &minute, &second) != 7) {
        return 0;
    }
    const int m = month_index(month);
    return m == 0 ? 0 : days_to_unix(year, m, day, hour, minute, second);
}

// ISO 8601 date used by the WebDAV Creation-Date header
// ("1997-12-23T10:00:00Z"). 0 when it does not parse.
int64_t parse_iso_date(const std::string& value) {
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    if (std::sscanf(value.c_str(), "%d-%d-%dT%d:%d:%d", &year, &month, &day, &hour, &minute,
                    &second) != 6) {
        return 0;
    }
    return days_to_unix(year, month, day, hour, minute, second);
}

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
    } else if (name == "creation-date") {
        cap->creation_date = value;
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
                std::string& error, std::string* blocked_url) {
    CURL* handle = curl_easy_init();
    if (!handle) {
        error = "cannot initialize curl";
        return false;
    }
    char errbuf[CURL_ERROR_SIZE] = {0};
    RequestOriginGuard origin_guard;
    apply_common_options(handle, o, headers, &origin_guard);
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

#if LIBCURL_VERSION_NUM < 0x075000
    if (origin_guard.guarded && cap.status >= 300 && cap.status < 400) {
        origin_guard.crossed_origin = true;
    }
#endif

    char* effective_ptr = nullptr;
    if (curl_easy_getinfo(handle, CURLINFO_EFFECTIVE_URL, &effective_ptr) == CURLE_OK &&
        effective_ptr) {
        effective_url = effective_ptr;
    }
    curl_easy_cleanup(handle);

    if (origin_guard.crossed_origin) {
        if (blocked_url) *blocked_url = origin_guard.blocked_url;
        error = kRedirectWithSensitiveHeadersError;
        return false;
    }
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
    RequestOriginGuard origin_guard;
    apply_common_options(handle, options, headers, &origin_guard);
    curl_easy_setopt(handle, CURLOPT_ERRORBUFFER, errbuf);
    curl_easy_setopt(handle, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, discard_all_cb);
    const CURLcode rc = curl_easy_perform(handle);

#if LIBCURL_VERSION_NUM < 0x075000
    if (origin_guard.guarded && out.status_code >= 300 && out.status_code < 400) {
        origin_guard.crossed_origin = true;
    }
#endif

    char* effective = nullptr;
    if (curl_easy_getinfo(handle, CURLINFO_EFFECTIVE_URL, &effective) == CURLE_OK && effective) {
        out.effective_url = effective;
    }
    curl_off_t length = -1;
    curl_easy_getinfo(handle, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &length);
    // libcurl asks for MDTM in the same probe, so FTP gets the server file
    // time without a second connection.
    curl_off_t filetime = -1;
    curl_easy_getinfo(handle, CURLINFO_FILETIME_T, &filetime);
    curl_easy_cleanup(handle);
    if (headers) curl_slist_free_all(headers);

    if (out.effective_url.empty()) out.effective_url = options.url;
    if (origin_guard.crossed_origin) {
        out.error = kRedirectWithSensitiveHeadersError;
        return out;
    }
    if (rc != CURLE_OK) {
        out.error = rc == CURLE_URL_MALFORMAT
                        ? kInvalidUrlError
                        : (errbuf[0] ? errbuf : curl_easy_strerror(rc));
        return out;
    }
    out.status_code = 0;  // no HTTP status over FTP
    out.content_length = length > 0 ? static_cast<uint64_t>(length) : 0;
    out.accept_ranges = out.content_length > 0;  // REST-based resume
    out.server_time = filetime > 0 ? static_cast<int64_t>(filetime) : 0;
    out.ok = true;
    return out;
}

// Follows redirects that the origin guard refused to send. libcurl has already
// resolved the target URL, so the retry only has to rebuild the request without
// the caller-provided metadata that must not reach another origin.
bool probe_follow(HttpOptions current, bool use_head, bool use_post, ProbeCapture& cap,
                  std::string& effective_url, std::string& error) {
    for (int attempt = 0; attempt <= kMaxRedirects; ++attempt) {
        cap = ProbeCapture{};
        std::string blocked_url;
        curl_slist* headers = build_headers(current.headers);
        const bool ok = probe_once(current, headers, use_head, use_post, cap, effective_url,
                                   error, &blocked_url);
        if (headers) curl_slist_free_all(headers);
        if (ok) return true;
        if (blocked_url.empty()) return false;
        if (!is_supported_download_url(blocked_url)) {
            error = kInvalidUrlError;
            return false;
        }
        if (use_post && !current.post_data.empty()) {
            // A redirect that keeps the method would resend the body to the new
            // origin; fail closed. 301/302/303 turn a POST into a GET.
            if (cap.status != 301 && cap.status != 302 && cap.status != 303) {
                error = kRedirectWithSensitiveHeadersError;
                return false;
            }
            current.post_data.clear();
            use_post = false;
        }
        current.url = blocked_url;
        current.headers.clear();
        current.referer.clear();
    }
    error = "too many redirects";
    return false;
}

// Probe without interstitial handling: HEAD first, GET Range: 0-0 as fallback.
ProbeResult probe_url_once(const HttpOptions& options) {
    if (is_ftp_url(options.url)) return probe_ftp(options);
    ensure_curl_global();
    ProbeResult out;

    ProbeCapture cap;
    std::string error;
    std::string effective_url = options.url;

    // 1) HEAD (unless POST data requires a POST probe).
    if (options.post_data.empty()) {
        if (probe_follow(options, /*use_head=*/true, /*use_post=*/false, cap, effective_url,
                         error)) {
            if (cap.status >= 200 && cap.status < 400) {
                // usable
            } else {
                cap = ProbeCapture{};  // fall through to GET range probe
            }
        } else {
            cap = ProbeCapture{};  // GET may still succeed where HEAD did not
        }
    }

    // 2) GET with Range: 0-0 (or POST).
    if (!cap.got_status) {
        const bool use_post = !options.post_data.empty();
        if (!probe_follow(options, /*use_head=*/false, use_post, cap, effective_url, error)) {
            out.error = error.empty() ? "probe failed" : error;
            return out;
        }
    }

    out.status_code = cap.status;
    out.effective_url = effective_url.empty() ? options.url : effective_url;
    out.accept_ranges = cap.accept_ranges;
    out.filename_hint = cap.filename_hint;
    out.mime_type = cap.content_type;
    const size_t semi = out.mime_type.find(';');
    if (semi != std::string::npos) out.mime_type = trim(out.mime_type.substr(0, semi));
    out.etag = cap.etag;
    out.last_modified = cap.last_modified;
    out.server_time = parse_iso_date(cap.creation_date);
    if (out.server_time == 0) out.server_time = parse_http_date(cap.last_modified);

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

}  // namespace swiftdm::detail

namespace swiftdm {

using namespace detail;

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

}  // namespace swiftdm
