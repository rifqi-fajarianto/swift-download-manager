#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "net/http.h"
#include "store/settings.h"

namespace swiftdm {

struct DownloadRequest {
    std::string url;
    std::string output_dir;        // empty = settings.download_dir
    std::string filename;          // empty = from Content-Disposition / URL
    int connections = 0;           // 0 = settings.max_connections
    int bandwidth_limit_kbps = 0;  // 0 = settings.bandwidth_limit_kbps (itself 0 = unlimited)
    bool resume = true;
    std::vector<HttpHeader> headers;
    std::string referer;
    std::string user_agent;
    std::string post_data;
};

struct ProgressInfo {
    uint64_t done = 0;
    uint64_t total = 0;  // 0 = unknown
    double speed_bps = 0.0;
    int percent = 0;
};

struct DownloadResult {
    enum class Status { Completed, Paused, Failed };
    Status status = Status::Failed;
    std::string path;   // final path (Completed) or part path (Paused/Failed)
    std::string error;  // "HTTP/1.1 403 Forbidden" style for Failed
    uint64_t size = 0;
};

// Temporary workspace for a download, stable across runs so resume works.
struct DownloadWorkspace {
    std::string dir;
    std::string part_path;
    std::string state_path;  // segments.json
};

DownloadWorkspace make_workspace(const Settings& settings, const std::string& url,
                                 const std::string& filename);

// Returns `path` if free, else "name (1).ext", "name (2).ext", ...
std::string unique_path(const std::string& path);

// Runs one download to completion (blocking). The optional progress callback
// returns false to pause; state is persisted so a later run() resumes.
class DownloadEngine {
  public:
    explicit DownloadEngine(Settings settings);

    const Settings& settings() const { return settings_; }

    DownloadResult run(const DownloadRequest& request,
                       const std::function<bool(const ProgressInfo&)>& on_progress = nullptr);

  private:
    Settings settings_;
};

}  // namespace swiftdm
