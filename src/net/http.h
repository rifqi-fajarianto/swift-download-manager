#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "net/rate_limit.h"

#include "engine/segment_plan.h"
#include "store/settings.h"

namespace swiftdm {

inline constexpr const char* kInvalidUrlError =
    "invalid download URL - include the http://, https://, ftp:// or ftps:// prefix";

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
    // URL the transfer has to use when it differs from the requested one
    // (a resolved download interstitial, or a normalised site link). Empty
    // means "the requested URL is already correct".
    std::string download_url;
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

// Extracts the status code from an error string produced by this module
// ("HTTP/1.1 403 Forbidden - ..."), 0 when the string is not an HTTP failure.
int http_error_status(const std::string& error);

// True for ftp:// and ftps:// URLs (case-insensitive).
bool is_ftp_url(const std::string& url);

// Probes a URL: HEAD first, falling back to GET Range: 0-0 (or POST when
// post_data is set). FTP URLs use a NOBODY (SIZE) probe instead. Fills
// size/range support/validators/filename hint.
//
// Known download interstitials are resolved first (docs/spec/04-protocols.md
// §5): Google Drive file/viewer links are rewritten to the direct download URL
// and the site's virus-scan notice is confirmed automatically, so the result
// describes the file rather than the page. Links that never lead to a file
// (Drive folder listings, sign-in walls, Docs editor pages) fail with a reason.
ProbeResult probe_url(const HttpOptions& options);

// Fetches a small resource into memory (HLS playlists). Fails when the body
// exceeds `max_bytes`. `effective_url` receives the post-redirect URL so
// relative playlist URIs resolve correctly.
bool fetch_text(const HttpOptions& options, std::string* text, std::string* effective_url,
                std::string* error, uint64_t max_bytes = 2u << 20);

enum class TransferOutcome { Completed, Paused, Failed };

// Sequential (non-ranging) download for servers without a known size or range
// support, writing from offset 0. The progress callback returns false to
// pause. `bytes_written` receives the bytes stored on disk (if provided).
// `resume_from` > 0 continues an interrupted transfer at that offset (FTP
// REST, or HTTP Range for servers without range support).
// The shared bandwidth control can be changed while the transfer is active;
// a zero rate means unlimited.
TransferOutcome stream_download(const HttpOptions& options, int write_fd,
                                const std::function<bool(uint64_t done)>& on_progress,
                                uint64_t* bytes_written, std::string* error,
                                uint64_t resume_from = 0,
                                std::shared_ptr<BandwidthLimit> bandwidth_limit = {});

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

    void set_bandwidth_limit(std::shared_ptr<BandwidthLimit> bandwidth_limit);
    void set_one_by_one(bool one_by_one);                   // ConnectionsAtOnce
    void set_max_retries(int retries);

    TransferOutcome run(const ProgressFn& on_progress, std::string* error);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace swiftdm
