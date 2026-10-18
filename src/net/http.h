#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "engine/segment_plan.h"
#include "store/settings.h"

namespace swiftdm {

struct HttpHeader {
    std::string name;
    std::string value;
};

// Browser cookies retain their URL scope so libcurl does not forward them to
// unrelated hosts when following redirects.
struct HttpCookie {
    std::string domain;
    std::string path = "/";
    std::string name;
    std::string value;
    bool secure = false;
    int64_t expires = 0;  // Unix seconds; 0 means a session cookie
};

struct ProbeResult {
    bool ok = false;
    long status_code = 0;
    std::string effective_url;    // after redirects
    uint64_t content_length = 0;  // total size (0 = unknown)
    bool accept_ranges = false;
    std::string filename_hint;    // Content-Disposition, decoded
    std::string mime_type;
    std::string etag;
    std::string last_modified;
    std::string error;            // "HTTP/1.1 403 Forbidden" or network error
};

// Transport options shared by probe and segment transfers.
struct HttpOptions {
    std::string url;
    std::vector<HttpHeader> headers;
    std::vector<HttpCookie> cookies;
    std::string user_agent;
    std::string referer;
    std::string proxy_type;  // "none"/"http"/"socks4"/"socks5"
    std::string proxy_address;
    int proxy_port = 0;
    std::string proxy_user;
    std::string proxy_password;
    std::string post_data;   // when set, probe uses POST and transfers are single-connection
    int connect_timeout_seconds = 30;
    int low_speed_seconds = 60;
};

// Human-readable HTTP reason phrase (subset; "" when unknown).
std::string http_reason_phrase(long status);

// Probes a URL: HEAD first, falling back to GET Range: 0-0 (or POST when
// post_data is set). Fills size/range support/validators/filename hint.
ProbeResult probe_url(const HttpOptions& options);

enum class TransferOutcome { Completed, Paused, Failed };

// Sequential (non-ranging) download for servers without a known size or range
// support, writing from offset 0. The progress callback returns false to
// pause. `bytes_written` receives the bytes stored on disk (if provided).
TransferOutcome stream_download(const HttpOptions& options, int write_fd,
                                const std::function<bool(uint64_t done)>& on_progress,
                                uint64_t* bytes_written, std::string* error);

// Downloads the unfinished ranges of `state` into `write_fd` (positioned
// writes at absolute offsets) using libcurl multi with parallel connections.
class SegmentDownloader {
  public:
    // Called about 4 times per second. Returning false requests a pause.
    using ProgressFn =
        std::function<bool(uint64_t done, uint64_t total, double speed_bps)>;

    SegmentDownloader(const HttpOptions& options, TransferState& state, int write_fd);
    ~SegmentDownloader();

    SegmentDownloader(const SegmentDownloader&) = delete;
    SegmentDownloader& operator=(const SegmentDownloader&) = delete;

    void set_bandwidth_limit_bytes(int64_t bytes_per_sec);  // 0 = unlimited
    void set_one_by_one(bool one_by_one);                   // ConnectionsAtOnce
    void set_max_retries(int retries);

    TransferOutcome run(const ProgressFn& on_progress, std::string* error);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace swiftdm
