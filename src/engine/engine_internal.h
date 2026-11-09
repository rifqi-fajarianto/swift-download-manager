#pragma once

#include <functional>
#include <memory>
#include <string>

#include "engine/download_engine.h"
#include "net/http.h"
#include "net/rate_limit.h"

namespace swiftdm {

// Picks a free final name and moves the part file there in one atomic claim, so
// a concurrent writer can never be overwritten. Defined in download_engine.cpp
// because the segmented path shares the same claiming loop.
bool move_file_unique(const std::string& from, const std::string& dir,
                      const std::string& filename, std::string* final_path,
                      std::string* error);

// Same, but replaces an existing file (redownload).
bool move_file_replace(const std::string& from, const std::string& dir,
                       const std::string& filename, std::string* final_path,
                       std::string* error);

namespace engine_internal {

// The per-protocol transfer paths of the engine, shared between
// download_engine.cpp (the dispatcher) and engine_ftp_hls.cpp (the FTP and HLS
// paths). They used to be helpers in the anonymous namespace of a single
// translation unit; they are internal to the engine and not part of the public
// DownloadEngine interface.

// Moves a finished part file onto its final name. Redownload replaces the
// existing file; every other run claims a free name so nothing is overwritten.
bool finalize_part_file(const std::string& from, const std::string& dir,
                        const std::string& filename, bool overwrite,
                        std::string* final_path, std::string* error);

// FTP: single connection, REST-based resume (spec 04-protocols.md §2).
DownloadResult run_ftp(const HttpOptions& options, const ProbeResult& probe,
                       const std::string& filename, const std::string& out_dir,
                       const DownloadWorkspace& ws, bool allow_resume, bool overwrite,
                       const std::shared_ptr<BandwidthLimit>& bandwidth_limit,
                       const std::function<bool(const ProgressInfo&)>& on_progress);

// HLS: fetch the playlist, then every segment, concatenated into one file.
// Resume continues at the first segment that was not fully written.
DownloadResult run_hls(const HttpOptions& base_options, const ProbeResult& probe,
                       const std::string& filename, const std::string& out_dir,
                       const DownloadWorkspace& ws, bool allow_resume, bool overwrite,
                       const std::shared_ptr<BandwidthLimit>& bandwidth_limit,
                       const std::function<bool(const ProgressInfo&)>& on_progress);

}  // namespace engine_internal
}  // namespace swiftdm
