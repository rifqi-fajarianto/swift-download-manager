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
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/file_io.h"
#include "core/log.h"
#include "core/util.h"
#include "net/interstitial.h"
#include "net/retry_policy.h"

#include "net/http_internal.h"

namespace swiftdm {

using namespace detail;

// ---------------------------------------------------------------------------
// SegmentDownloader
// ---------------------------------------------------------------------------

struct SegmentDownloader::Impl {
    struct HandleCtx {
        Impl* impl = nullptr;
        // Stable key of the segment this handle serves (Segment::start). Only
        // `end` and `done` of a segment ever change, so the start offset keeps
        // pointing at the right range even after the plan is split.
        uint64_t seg_start = 0;
        long status = 0;
        bool range_bad = false;
        bool content_range_bad = false;
        bool content_range_seen = false;
        bool content_range_valid = false;
        bool if_range_sent = false;
        uint64_t content_range_start = 0;
        uint64_t content_range_end = 0;
        uint64_t content_range_total = 0;
        // Range end actually requested in this attempt. A running segment can be
        // split behind its back, which shrinks seg.end; comparing the response
        // against this value keeps the Content-Range check honest, and the
        // mismatch tells write_cb to stop the connection on purpose.
        uint64_t requested_end = 0;
        bool split_stop = false;
        // First body byte of the current attempt; 0 = no byte yet. The split
        // policy measures warm-up from here, not from the request start.
        int64_t first_byte_ms = 0;
        bool validator_changed = false;
        bool interrupted = false;
        int64_t retry_after = 0;  // seconds advertised by the host, 0 = none
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
    // Parallel ranges the host currently tolerates. Narrowed whenever it
    // refuses one more connection (docs/spec/03-engine.md §5).
    AdaptiveConcurrency concurrency{1};

    struct SegRuntime {
        int attempts = 0;
        int64_t next_try_ms = 0;
        bool active = false;
        bool done = false;
        CURL* easy = nullptr;
        curl_slist* request_headers = nullptr;
        std::string error_buffer;
    };

    // One per segment, allocated on the heap so its address stays valid while
    // the segment table grows. Keyed by Segment::start; the plan stays sorted by
    // start, so the map order matches the plan order.
    struct Worker {
        HandleCtx ctx;
        SegRuntime rt;
    };
    std::map<uint64_t, std::unique_ptr<Worker>> workers;

    Worker& worker_at(uint64_t start) { return *workers.at(start); }
    const Worker& worker_at(uint64_t start) const { return *workers.at(start); }

    Segment& seg_of(uint64_t start) {
        auto it = std::lower_bound(
            state.segments.begin(), state.segments.end(), start,
            [](const Segment& s, uint64_t v) { return s.start < v; });
        return *it;  // the caller only passes starts that exist in the plan
    }

    // Rebuilds one Worker per segment. Called on every run() because the engine
    // may run the same downloader twice (single-connection fallback).
    void build_workers() {
        workers.clear();
        for (const Segment& s : state.segments) {
            auto w = std::make_unique<Worker>();
            w->ctx.impl = this;
            w->ctx.seg_start = s.start;
            workers.emplace(s.start, std::move(w));
        }
    }

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
        if (ctx->first_byte_ms == 0) ctx->first_byte_ms = now_ms();
        Segment& seg = impl->seg_of(ctx->seg_start);
        if (seg.done >= seg.length()) {
            // The range is complete for this segment. When the plan shrank
            // behind this request (a split), the rest of the body belongs to the
            // new segment: stop the connection on purpose.
            if (seg.end < ctx->requested_end) {
                ctx->split_stop = true;
                return 0;
            }
            return total;  // nothing left; discard
        }
        uint64_t to_write = total;
        const uint64_t remaining = seg.length() - seg.done;
        if (to_write > remaining) to_write = remaining;
        const uint64_t offset = seg.start + seg.done;
        // write_file_at() retries on EINTR and finishes short writes itself, so
        // only a negative result is a real disk failure.
        const std::ptrdiff_t written = write_file_at(impl->fd, ptr, to_write, offset);
        if (written < 0 || static_cast<uint64_t>(written) != to_write) return 0;
        seg.done += to_write;
        if (seg.finished() && seg.end < ctx->requested_end) {
            ctx->split_stop = true;  // tail now belongs to another segment
            return 0;                // curl reports CURLE_WRITE_ERROR
        }
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
                const Segment& seg = ctx->impl->seg_of(ctx->seg_start);
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
        } else if (starts_with_ci(line, "retry-after")) {
            const size_t colon = line.find(':');
            if (colon != std::string::npos) {
                ctx->retry_after = parse_retry_after_seconds(line.substr(colon + 1));
            }
        } else if (starts_with_ci(line, "content-range")) {
            const size_t colon = line.find(':');
            if (colon != std::string::npos) {
                ctx->content_range_seen = true;
                ctx->content_range_valid = parse_content_range(
                    line.substr(colon + 1), &ctx->content_range_start,
                    &ctx->content_range_end, &ctx->content_range_total);
                const Segment& seg = ctx->impl->seg_of(ctx->seg_start);
                const uint64_t expected_start = seg.start + seg.done;
                if (ctx->status == 206 &&
                    (!ctx->content_range_valid ||
                     ctx->content_range_start != expected_start ||
                     ctx->content_range_end != ctx->requested_end ||
                     ctx->content_range_total != ctx->impl->state.file_size)) {
                    ctx->content_range_bad = true;
                }
            }
        }
        return len;
    }

    bool start_segment(Worker& w) {
        Segment& seg = seg_of(w.ctx.seg_start);
        if (seg.finished()) {
            w.rt.done = true;
            return false;
        }
        CURL* handle = curl_easy_init();
        if (!handle) return false;
        HandleCtx& ctx = w.ctx;
        ctx.impl = this;
        ctx.status = 0;
        ctx.range_bad = false;
        ctx.content_range_bad = false;
        ctx.content_range_seen = false;
        ctx.content_range_valid = false;
        ctx.if_range_sent = false;
        ctx.content_range_start = 0;
        ctx.content_range_end = 0;
        ctx.content_range_total = 0;
        ctx.requested_end = seg.end;
        ctx.split_stop = false;
        ctx.first_byte_ms = 0;
        ctx.validator_changed = false;
        ctx.interrupted = false;
        ctx.retry_after = 0;
        ctx.content_type.clear();
        ctx.error_body.clear();
        ctx.origin_guard = RequestOriginGuard{};

        w.rt.error_buffer.assign(CURL_ERROR_SIZE, '\0');
        w.rt.request_headers = build_headers(options.headers);
        std::string if_range;
        if (!state.etag.empty() && state.etag.rfind("W/", 0) != 0) {
            if_range = "If-Range: " + state.etag;
        } else if (!state.last_modified.empty()) {
            if_range = "If-Range: " + state.last_modified;
        }
        if (!if_range.empty()) {
            ctx.if_range_sent = true;
            curl_slist* appended = curl_slist_append(w.rt.request_headers, if_range.c_str());
            if (!appended) {
                curl_slist_free_all(w.rt.request_headers);
                w.rt.request_headers = nullptr;
                curl_easy_cleanup(handle);
                return false;
            }
            w.rt.request_headers = appended;
        }
        apply_common_options(handle, options, w.rt.request_headers, &ctx.origin_guard);
#if LIBCURL_VERSION_NUM < 0x075000
        if (ctx.origin_guard.guarded) {
            // With older libcurl, follow no redirects on requests that carry
            // caller-provided headers/referer (fail closed).
            curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 0L);
        }
#endif
        curl_easy_setopt(handle, CURLOPT_ERRORBUFFER, w.rt.error_buffer.data());
        curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, write_cb);
        curl_easy_setopt(handle, CURLOPT_WRITEDATA, &ctx);
        curl_easy_setopt(handle, CURLOPT_HEADERFUNCTION, header_cb);
        curl_easy_setopt(handle, CURLOPT_HEADERDATA, &ctx);
        // The completion message only carries the easy handle; this lets the
        // multi loop find the worker again without scanning the plan.
        curl_easy_setopt(handle, CURLOPT_PRIVATE, &w);
        const std::string range =
            std::to_string(seg.start + seg.done) + "-" + std::to_string(seg.end);
        curl_easy_setopt(handle, CURLOPT_RANGE, range.c_str());

        if (curl_multi_add_handle(multi, handle) != CURLM_OK) {
            curl_slist_free_all(w.rt.request_headers);
            w.rt.request_headers = nullptr;
            curl_easy_cleanup(handle);
            return false;
        }
        w.rt.easy = handle;
        w.rt.active = true;
        ++w.rt.attempts;
        return true;
    }

    // Returns: 0 = retry scheduled, 1 = terminal error, 2 = interrupted,
    // 3 = deferred because the host refused one more parallel connection.
    int finalize_segment(Worker& w, CURLcode rc, std::string& error) {
        SegRuntime& rt = w.rt;
        CURL* handle = rt.easy;
        rt.easy = nullptr;
        rt.active = false;
        long code = 0;
        curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &code);
        curl_multi_remove_handle(multi, handle);
        char* effective_url = nullptr;
        if (curl_easy_getinfo(handle, CURLINFO_EFFECTIVE_URL, &effective_url) == CURLE_OK &&
            effective_url && w.ctx.origin_guard.guarded &&
            request_origin(effective_url) != w.ctx.origin_guard.original_origin) {
            w.ctx.origin_guard.crossed_origin = true;
        }
        curl_easy_cleanup(handle);
        if (rt.request_headers) {
            curl_slist_free_all(rt.request_headers);
            rt.request_headers = nullptr;
        }

        Segment& seg = seg_of(w.ctx.seg_start);
        if (w.ctx.interrupted) {
            error = "transfer interrupted";
            return 2;
        }
        // A split shrank this segment while the request was already running; the
        // write callback ended the transfer on purpose, so this is success.
        if (w.ctx.split_stop && seg.finished()) {
            rt.done = true;
            concurrency.on_success();
            return 0;
        }
        if (w.ctx.content_range_bad) {
            error = "server returned an invalid Content-Range for a segment";
            return 1;
        }
        if (w.ctx.range_bad) {
            error = w.ctx.validator_changed
                        ? "the remote file changed during the download; restart is required"
                        : "server does not support range requests";
            return 1;
        }
        if (code == 206 && !w.ctx.content_range_seen) {
            error = "server returned an invalid Content-Range for a segment";
            return 1;
        }
        if (rc == CURLE_OK && code >= 200 && code < 300 && seg.finished()) {
            rt.done = true;
            concurrency.on_success();
            return 0;
        }
        if (w.ctx.origin_guard.crossed_origin ||
            (w.ctx.origin_guard.guarded && code >= 300 && code < 400)) {
            error = kRedirectWithSensitiveHeadersError;
            return 1;
        }
        if (rc == CURLE_WRITE_ERROR && !w.ctx.range_bad) {
            error = "disk write failed";
            return 1;
        }
        // The host refused this range because too many connections were open
        // (UploadHaven's "Download In Progress", pixeldrain's
        // max_concurrent_downloads). Retrying with the same budget cannot help:
        // the other ranges keep the slots busy for the whole backoff.
        if (is_concurrency_refusal(code, w.ctx.error_body, w.ctx.content_type)) {
            // 403 is the host stating the rule ("max_concurrent_downloads"):
            // the parallel plan is unusable, so fail at once and let the engine
            // retry the remaining ranges on a single connection. 503 is
            // "temporarily unavailable": the slot is busy for a while, so narrow
            // the plan and wait the range out instead of failing the download.
            if (code == 403 || concurrency.exhausted()) {
                error = http_error_with_detail(code, w.ctx.error_body, w.ctx.content_type);
                return 1;
            }
            if (concurrency.limit() > 1) concurrency.on_refusal();
            concurrency.on_deferral();
            // The refused request never had a fair chance, so it does not spend
            // a retry. Deferrals have their own (larger) cap.
            if (rt.attempts > 0) --rt.attempts;
            const int64_t backoff = w.ctx.retry_after > 0
                                        ? w.ctx.retry_after * 1000
                                        : AdaptiveConcurrency::defer_backoff_ms(
                                              concurrency.deferrals());
            rt.next_try_ms = now_ms() + backoff;
            log_warn("segment @" + std::to_string(w.ctx.seg_start) +
                     ": host refused a parallel connection, parallel limit -> " +
                     std::to_string(concurrency.limit()) + ", retry in " +
                     std::to_string(backoff) + " ms");
            return 3;
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
                error = http_error_with_detail(code, w.ctx.error_body, w.ctx.content_type);
            } else if (rc == CURLE_URL_MALFORMAT) {
                error = kInvalidUrlError;
            } else if (!rt.error_buffer.empty() && rt.error_buffer[0] != '\0') {
                error = rt.error_buffer.c_str();
            } else {
                error = curl_url_error(rc);
            }
            return 1;
        }
        // Spec 03-engine.md §5: 1s, 2s, 4s, 8s, 16s, capped at 60 s. A host that
        // sends Retry-After knows better than our schedule.
        const int64_t backoff = w.ctx.retry_after > 0
                                    ? w.ctx.retry_after * 1000
                                    : std::min<int64_t>(60000, 1000LL << (rt.attempts - 1));
        rt.next_try_ms = now_ms() + backoff;
        log_warn("segment @" + std::to_string(w.ctx.seg_start) + " retry " +
                 std::to_string(rt.attempts) + "/" + std::to_string(max_retries) + " in " +
                 std::to_string(backoff) + " ms");
        return 0;
    }

    void cleanup_handles() {
        for (auto& entry : workers) {
            SegRuntime& rt = entry.second->rt;
            if (rt.easy) {
                curl_multi_remove_handle(multi, rt.easy);
                curl_easy_cleanup(rt.easy);
                rt.easy = nullptr;
                rt.active = false;
            }
            if (rt.request_headers) {
                curl_slist_free_all(rt.request_headers);
                rt.request_headers = nullptr;
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
    impl.build_workers();

    // The parallel budget starts at the plan size and only narrows from there
    // (docs/spec/03-engine.md §5). A resumed download therefore re-learns the
    // host's limit: the refusals cost one deferral round (a few seconds), not a
    // failed download.
    impl.concurrency = AdaptiveConcurrency(impl.one_by_one
                                               ? 1
                                               : static_cast<int>(impl.state.segments.size()));

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
        for (const auto& entry : impl.workers) {
            if (entry.second->rt.active) ++active;
        }
        const int parallel_limit = std::max(1, impl.concurrency.limit());
        for (auto& entry : impl.workers) {
            Impl::Worker& w = *entry.second;
            Impl::SegRuntime& rt = w.rt;
            Segment& seg = impl.seg_of(w.ctx.seg_start);
            if (rt.active || rt.done || seg.finished()) {
                if (seg.finished() && !rt.active) rt.done = true;
                continue;
            }
            if (rt.next_try_ms > now) continue;
            if (active >= parallel_limit) break;
            if (impl.start_segment(w)) ++active;
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
            for (const auto& entry : impl.workers) {
                const Impl::SegRuntime& rt = entry.second->rt;
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
                void* priv = nullptr;
                if (curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, &priv) != CURLE_OK ||
                    !priv) {
                    continue;
                }
                auto* worker = static_cast<Impl::Worker*>(priv);
                std::string seg_error;
                const int finalized = impl.finalize_segment(*worker, msg->data.result, seg_error);
                if (finalized == 2) {
                    outcome = TransferOutcome::Paused;
                    goto finish;
                }
                if (finalized == 1) {
                    failure = seg_error;
                    outcome = TransferOutcome::Failed;
                    goto finish;
                }
                // finalized == 3: deferred until the narrowed plan has a free
                // slot; next_try_ms was set by finalize_segment.
            }
            // Do not sleep past the next report, otherwise an idle poll drags
            // the progress cadence below kReportIntervalMs.
            const int64_t to_report = kReportIntervalMs - (now_ms() - last_report);
            const int poll_ms = static_cast<int>(
                std::max<int64_t>(1, std::min<int64_t>(kIoTimeoutMs, to_report)));
            curl_multi_poll(impl.multi, nullptr, 0, poll_ms, nullptr);
        }

        // Speed sampling and progress/pause reporting.
        const int64_t t = now_ms();
        const uint64_t done = impl.done_bytes();
        if (t - last_sample >= kSpeedSampleMs) {
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
