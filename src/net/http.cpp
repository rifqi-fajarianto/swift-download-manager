#include "net/http.h"

#include <curl/curl.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "core/log.h"
#include "core/util.h"

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

curl_slist* build_headers(const std::vector<HttpHeader>& headers) {
    curl_slist* list = nullptr;
    for (const HttpHeader& h : headers) {
        if (h.name.empty()) continue;
        list = curl_slist_append(list, (h.name + ": " + h.value).c_str());
    }
    return list;
}

void apply_common_options(CURL* handle, const HttpOptions& o, curl_slist* headers) {
    curl_easy_setopt(handle, CURLOPT_URL, o.url.c_str());
    curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
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

size_t probe_discard_cb(char*, size_t, size_t, void*) {
    return 0;  // abort as soon as the body starts; headers are already captured
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
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, probe_discard_cb);
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
        else error = curl_easy_strerror(rc);
        return false;
    }
    // Writing is aborted intentionally once the body starts.
    if (rc != CURLE_OK && rc != CURLE_WRITE_ERROR) {
        error = curl_easy_strerror(rc);
        return false;
    }
    error.clear();
    return true;
}

}  // namespace

ProbeResult probe_url(const HttpOptions& options) {
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
        out.error = http_error_text(cap.status);
        return out;
    }
    out.content_length = cap.content_range_total;
    if (out.content_length == 0) out.content_length = cap.content_length;
    out.ok = true;
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
    };

    HttpOptions options;
    TransferState& state;
    int fd;

    CURLM* multi = nullptr;
    curl_slist* headers = nullptr;

    int64_t limit_bytes = 0;
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

    void apply_speed_limit() {
        if (limit_bytes <= 0) return;
        size_t active = 0;
        for (const auto& r : runtime) {
            if (r.active) ++active;
        }
        if (active == 0) return;
        int64_t per = limit_bytes / static_cast<int64_t>(active);
        if (per < 1024) per = 1024;
        for (size_t i = 0; i < runtime.size(); ++i) {
            if (runtime[i].active && runtime[i].easy) {
                curl_easy_setopt(runtime[i].easy, CURLOPT_MAX_RECV_SPEED_LARGE,
                                 static_cast<curl_off_t>(per));
            }
        }
    }

    static size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
        auto* ctx = static_cast<HandleCtx*>(userdata);
        Impl* impl = ctx->impl;
        const size_t total = size * nmemb;
        if (total == 0 || ctx->range_bad) return 0;
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
        if (len >= 5 && std::memcmp(buffer, "HTTP/", 5) == 0) {
            const std::string line(buffer, len);
            const size_t sp = line.find(' ');
            if (sp != std::string::npos) {
                ctx->status = std::strtol(line.c_str() + sp + 1, nullptr, 10);
                const Segment& seg = ctx->impl->state.segments[ctx->index];
                const uint64_t from = seg.start + seg.done;
                if (ctx->status == 200 && from > 0) ctx->range_bad = true;
            }
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

    // Returns: 0 = retry scheduled, 1 = terminal error (message in `error`).
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
            if (code > 0) error = http_error_text(code);
            else if (!rt.error_buffer.empty() && rt.error_buffer[0] != '\0') error = rt.error_buffer.c_str();
            else error = curl_easy_strerror(rc);
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

void SegmentDownloader::set_bandwidth_limit_bytes(int64_t bytes_per_sec) {
    impl_->limit_bytes = bytes_per_sec;
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
        impl.apply_speed_limit();

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
                if (impl.finalize_segment(idx, msg->data.result, seg_error) == 1) {
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
    const std::function<bool(uint64_t)>* on_progress = nullptr;
    int64_t last_report = 0;
};

size_t stream_write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* ctx = static_cast<StreamCtx*>(userdata);
    const size_t total = size * nmemb;
    if (total == 0) return 0;
    const ssize_t written = ::write(ctx->fd, ptr, total);
    if (written < 0 || static_cast<size_t>(written) != total) return 0;
    ctx->written += total;
    return total;
}

int stream_progress_cb(void* clientp, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    auto* ctx = static_cast<StreamCtx*>(clientp);
    const int64_t now = now_ms();
    if (now - ctx->last_report >= kReportIntervalMs) {
        ctx->last_report = now;
        if (ctx->on_progress && !(*ctx->on_progress)(ctx->written)) return 1;  // pause
    }
    return 0;
}

}  // namespace

TransferOutcome stream_download(const HttpOptions& options, int write_fd,
                                const std::function<bool(uint64_t done)>& on_progress,
                                uint64_t* bytes_written, std::string* error) {
    ensure_curl_global();
    curl_slist* headers = build_headers(options.headers);
    CURL* handle = curl_easy_init();
    if (!handle) {
        if (headers) curl_slist_free_all(headers);
        if (error) *error = "cannot initialize curl";
        return TransferOutcome::Failed;
    }

    StreamCtx ctx;
    ctx.fd = write_fd;
    ctx.on_progress = &on_progress;
    ctx.last_report = now_ms();

    char errbuf[CURL_ERROR_SIZE] = {0};
    apply_common_options(handle, options, headers);
    curl_easy_setopt(handle, CURLOPT_ERRORBUFFER, errbuf);
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, stream_write_cb);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(handle, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(handle, CURLOPT_XFERINFOFUNCTION, stream_progress_cb);
    curl_easy_setopt(handle, CURLOPT_XFERINFODATA, &ctx);
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
    if (bytes_written) *bytes_written = ctx.written;

    if (rc == CURLE_ABORTED_BY_CALLBACK) return TransferOutcome::Paused;
    if (rc != CURLE_OK) {
        if (error) *error = errbuf[0] ? errbuf : curl_easy_strerror(rc);
        return TransferOutcome::Failed;
    }
    if (code >= 400) {
        if (error) *error = http_error_text(code);
        return TransferOutcome::Failed;
    }
    if (error) error->clear();
    return TransferOutcome::Completed;
}

}  // namespace swiftdm
