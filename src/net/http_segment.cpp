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

namespace swiftdm {

using namespace detail;

// ---------------------------------------------------------------------------
// SegmentDownloader
// ---------------------------------------------------------------------------

struct SegmentDownloader::Impl {
    struct HandleCtx {
        Impl* impl = nullptr;
        size_t index = 0;
        long status = 0;
        bool range_bad = false;
        bool content_range_bad = false;
        bool content_range_seen = false;
        bool content_range_valid = false;
        bool if_range_sent = false;
        uint64_t content_range_start = 0;
        uint64_t content_range_end = 0;
        uint64_t content_range_total = 0;
        bool validator_changed = false;
        bool interrupted = false;
        std::string content_type;
        std::string error_body;  // start of a 4xx/5xx body, for the error message
        RequestOriginGuard origin_guard;
    };

    HttpOptions options;
    TransferState& state;
    int fd;

    CURLM* multi = nullptr;

    std::shared_ptr<BandwidthLimit> bandwidth_limit;
    bool one_by_one = false;
    int max_retries = kMaxRetries;

    struct SegRuntime {
        int attempts = 0;
        int64_t next_try_ms = 0;
        bool active = false;
        bool done = false;
        CURL* easy = nullptr;
        curl_slist* request_headers = nullptr;
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
        if (total == 0 || ctx->range_bad || ctx->content_range_bad) return 0;
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
        // pwrite may be interrupted or write fewer bytes than asked; a short
        // write is not a disk failure, so keep writing until the chunk is
        // complete.
        uint64_t left = to_write;
        uint64_t at = offset;
        while (left > 0) {
            const ssize_t written = ::pwrite(impl->fd, ptr + (to_write - left), left,
                                             static_cast<off_t>(at));
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) return 0;
            left -= static_cast<uint64_t>(written);
            at += static_cast<uint64_t>(written);
        }
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
                ctx->content_range_seen = false;
                ctx->content_range_valid = false;
                ctx->content_range_bad = false;
                ctx->validator_changed = false;
                const Segment& seg = ctx->impl->state.segments[ctx->index];
                const uint64_t from = seg.start + seg.done;
                if (ctx->status == 200 && from > 0) {
                    ctx->range_bad = true;
                    ctx->validator_changed = ctx->if_range_sent;
                }
            }
            return len;
        }
        if (line == "\r\n" || line == "\n") {
            if (ctx->status == 206 && !ctx->content_range_seen) {
                ctx->content_range_bad = true;
            }
        } else if (starts_with_ci(line, "content-type")) {
            const size_t colon = line.find(':');
            if (colon != std::string::npos) ctx->content_type = trim(line.substr(colon + 1));
        } else if (starts_with_ci(line, "content-range")) {
            const size_t colon = line.find(':');
            if (colon != std::string::npos) {
                ctx->content_range_seen = true;
                ctx->content_range_valid = parse_content_range(
                    line.substr(colon + 1), &ctx->content_range_start,
                    &ctx->content_range_end, &ctx->content_range_total);
                const Segment& seg = ctx->impl->state.segments[ctx->index];
                const uint64_t expected_start = seg.start + seg.done;
                if (ctx->status == 206 &&
                    (!ctx->content_range_valid ||
                     ctx->content_range_start != expected_start ||
                     ctx->content_range_end != seg.end ||
                     ctx->content_range_total != ctx->impl->state.file_size)) {
                    ctx->content_range_bad = true;
                }
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
        ctx.content_range_bad = false;
        ctx.content_range_seen = false;
        ctx.content_range_valid = false;
        ctx.if_range_sent = false;
        ctx.content_range_start = 0;
        ctx.content_range_end = 0;
        ctx.content_range_total = 0;
        ctx.validator_changed = false;
        ctx.interrupted = false;
        ctx.content_type.clear();
        ctx.error_body.clear();
        ctx.origin_guard = RequestOriginGuard{};

        runtime[i].error_buffer.assign(CURL_ERROR_SIZE, '\0');
        runtime[i].request_headers = build_headers(options.headers);
        std::string if_range;
        if (!state.etag.empty() && state.etag.rfind("W/", 0) != 0) {
            if_range = "If-Range: " + state.etag;
        } else if (!state.last_modified.empty()) {
            if_range = "If-Range: " + state.last_modified;
        }
        if (!if_range.empty()) {
            ctx.if_range_sent = true;
            curl_slist* appended = curl_slist_append(runtime[i].request_headers,
                                                     if_range.c_str());
            if (!appended) {
                curl_slist_free_all(runtime[i].request_headers);
                runtime[i].request_headers = nullptr;
                curl_easy_cleanup(handle);
                return false;
            }
            runtime[i].request_headers = appended;
        }
        apply_common_options(handle, options, runtime[i].request_headers, &ctx.origin_guard);
#if LIBCURL_VERSION_NUM < 0x075000
        if (ctx.origin_guard.guarded) {
            // With older libcurl, follow no redirects on requests that carry
            // caller-provided headers/referer (fail closed).
            curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 0L);
        }
#endif
        curl_easy_setopt(handle, CURLOPT_ERRORBUFFER, runtime[i].error_buffer.data());
        curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, write_cb);
        curl_easy_setopt(handle, CURLOPT_WRITEDATA, &ctx);
        curl_easy_setopt(handle, CURLOPT_HEADERFUNCTION, header_cb);
        curl_easy_setopt(handle, CURLOPT_HEADERDATA, &ctx);
        const std::string range =
            std::to_string(seg.start + seg.done) + "-" + std::to_string(seg.end);
        curl_easy_setopt(handle, CURLOPT_RANGE, range.c_str());

        if (curl_multi_add_handle(multi, handle) != CURLM_OK) {
            curl_slist_free_all(runtime[i].request_headers);
            runtime[i].request_headers = nullptr;
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
        char* effective_url = nullptr;
        if (curl_easy_getinfo(handle, CURLINFO_EFFECTIVE_URL, &effective_url) == CURLE_OK &&
            effective_url && contexts[i].origin_guard.guarded &&
            request_origin(effective_url) != contexts[i].origin_guard.original_origin) {
            contexts[i].origin_guard.crossed_origin = true;
        }
        curl_easy_cleanup(handle);
        if (rt.request_headers) {
            curl_slist_free_all(rt.request_headers);
            rt.request_headers = nullptr;
        }

        Segment& seg = state.segments[i];
        if (contexts[i].interrupted) {
            error = "transfer interrupted";
            return 2;
        }
        if (contexts[i].content_range_bad) {
            error = "server returned an invalid Content-Range for a segment";
            return 1;
        }
        if (contexts[i].range_bad) {
            error = contexts[i].validator_changed
                        ? "the remote file changed during the download; restart is required"
                        : "server does not support range requests";
            return 1;
        }
        const Segment& expected_segment = state.segments[i];
        if (code == 206 && !contexts[i].content_range_seen) {
            error = "server returned an invalid Content-Range for a segment";
            return 1;
        }
        if (rc == CURLE_OK && code >= 200 && code < 300 && seg.finished()) {
            rt.done = true;
            return 0;
        }
        if (contexts[i].origin_guard.crossed_origin ||
            (contexts[i].origin_guard.guarded && code >= 300 && code < 400)) {
            error = kRedirectWithSensitiveHeadersError;
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
            if (runtime[i].request_headers) {
                curl_slist_free_all(runtime[i].request_headers);
                runtime[i].request_headers = nullptr;
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
    if (impl.multi) {
        curl_multi_cleanup(impl.multi);
        impl.multi = nullptr;
    }
    if (error) *error = failure;
    return outcome;
}

}  // namespace swiftdm
