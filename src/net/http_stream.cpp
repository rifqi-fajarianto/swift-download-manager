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
    bool range_bad = false;
    bool content_range_seen = false;
    bool content_range_valid = false;
    bool if_range_sent = false;
    uint64_t content_range_start = 0;
    uint64_t content_range_end = 0;
    uint64_t content_range_total = 0;
};

size_t stream_write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* ctx = static_cast<StreamCtx*>(userdata);
    const size_t total = size * nmemb;
    if (total == 0) return 0;
    // Redirect and error bodies must not land in the output file.
    if (ctx->status >= 300 || ctx->range_bad) {
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
            ctx->content_range_seen = false;
            ctx->content_range_valid = false;
            if (ctx->status == 200 && ctx->base > 0) {
                ctx->range_bad = true;
            }
        }
        return len;
    }
    if (line == "\r\n" || line == "\n") {
        if (ctx->status == 206 && !ctx->content_range_seen) ctx->range_bad = true;
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
        if (ctx->status == 206 &&
            (!ctx->content_range_valid || ctx->content_range_start != ctx->base ||
             ctx->content_range_total <= ctx->content_range_end)) {
            ctx->range_bad = true;
        }
        }
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

}  // namespace swiftdm::detail

namespace swiftdm {

using namespace detail;

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
    RequestOriginGuard origin_guard;
    apply_common_options(handle, options, headers, &origin_guard);
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
        std::string if_range;
        if (!options.etag.empty() && options.etag.rfind("W/", 0) != 0) {
            if_range = "If-Range: " + options.etag;
        } else if (!options.last_modified.empty()) {
            if_range = "If-Range: " + options.last_modified;
        }
        if (!if_range.empty()) {
            curl_slist* appended = curl_slist_append(headers, if_range.c_str());
            if (!appended) {
                curl_easy_cleanup(handle);
                curl_slist_free_all(headers);
                if (error) *error = "cannot configure resume validator";
                return TransferOutcome::Failed;
            }
            headers = appended;
            ctx.if_range_sent = true;
            curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers);
        }
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
    if (ctx.range_bad ||
        (code == 206 && (!ctx.content_range_seen || !ctx.content_range_valid ||
                         ctx.content_range_start != resume_from ||
                         ctx.content_range_total <= ctx.content_range_end))) {
        curl_easy_cleanup(handle);
        if (headers) curl_slist_free_all(headers);
        if (bytes_written) *bytes_written = ctx.written + ctx.base;
        if (error) *error = "server returned an invalid range response while resuming";
        return TransferOutcome::Failed;
    }
    char* effective_url = nullptr;
    if (curl_easy_getinfo(handle, CURLINFO_EFFECTIVE_URL, &effective_url) == CURLE_OK &&
        effective_url && origin_guard.guarded &&
        request_origin(effective_url) != origin_guard.original_origin) {
        origin_guard.crossed_origin = true;
    }
    curl_easy_cleanup(handle);
    if (headers) curl_slist_free_all(headers);
    if (bytes_written) *bytes_written = ctx.written + ctx.base;

    if (origin_guard.crossed_origin ||
        (origin_guard.guarded && code >= 300 && code < 400)) {
        if (error) *error = kRedirectWithSensitiveHeadersError;
        return TransferOutcome::Failed;
    }
    if (rc == CURLE_ABORTED_BY_CALLBACK || ctx.interrupted) return TransferOutcome::Paused;
    if (rc != CURLE_OK) {
        if (error) {
            *error = origin_guard.crossed_origin
                         ? std::string(kRedirectWithSensitiveHeadersError)
                         : rc == CURLE_URL_MALFORMAT
                         ? kInvalidUrlError
                         : (errbuf[0] ? errbuf : curl_easy_strerror(rc));
        }
        return TransferOutcome::Failed;
    }
    if (code >= 400) {
        if (error) *error = http_error_with_detail(code, ctx.error_body, ctx.content_type);
        return TransferOutcome::Failed;
    }
    if (origin_guard.guarded && code >= 300 && code < 400) {
        if (error) *error = kRedirectWithSensitiveHeadersError;
        return TransferOutcome::Failed;
    }
    if (error) error->clear();
    return TransferOutcome::Completed;
}

}  // namespace swiftdm
