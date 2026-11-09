#include "engine/engine_internal.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include "core/file_io.h"
#include "core/util.h"
#include "engine/hls.h"

#ifndef _WIN32
#include <unistd.h>
#endif

namespace swiftdm {
namespace engine_internal {

// Moves a finished part file onto its final name. Redownload replaces the
// existing file; every other run claims a free name so nothing is overwritten.
bool finalize_part_file(const std::string& from, const std::string& dir,
                        const std::string& filename, bool overwrite,
                        std::string* final_path, std::string* error) {
    if (overwrite) {
        return swiftdm::move_file_replace(from, dir, filename, final_path, error);
    }
    return swiftdm::move_file_unique(from, dir, filename, final_path, error);
}

// ---------------------------------------------------------------------------
// FTP: single connection, REST-based resume (spec 04-protocols.md §2).
// ---------------------------------------------------------------------------
DownloadResult run_ftp(const HttpOptions& options, const ProbeResult& probe,
                       const std::string& filename, const std::string& out_dir,
                       const DownloadWorkspace& ws, bool allow_resume, bool overwrite,
                       const std::shared_ptr<BandwidthLimit>& bandwidth_limit,
                       const std::function<bool(const ProgressInfo&)>& on_progress) {
    DownloadResult result;
    result.workspace_dir = ws.dir;
    const bool known_size = probe.content_length > 0;

    uint64_t resume_from = 0;
    std::error_code ec;
    if (allow_resume && known_size && std::filesystem::exists(path_from_utf8(ws.part_path), ec)) {
        const uint64_t have = std::filesystem::file_size(path_from_utf8(ws.part_path), ec);
        if (!ec && have < probe.content_length) resume_from = have;
    }

    const unsigned flags = FileWrite | FileCreate |
                           (resume_from > 0 ? 0u : FileTruncate);
    const int fd = open_file(ws.part_path, flags);
    if (fd < 0) {
        result.status = DownloadResult::Status::Failed;
        result.error = "cannot create '" + ws.part_path + "': " + errno_string(errno);
        return result;
    }

    ProgressInfo info;
    info.total = probe.content_length;
    double stream_speed = 0.0;
    auto progress = [&](uint64_t done) -> bool {
        info.done = done;
        info.speed_bps = stream_speed;
        info.percent = percent_of(done, probe.content_length);
        return on_progress ? on_progress(info) : true;
    };
    uint64_t written = 0;
    std::string transfer_error;
    const TransferOutcome outcome =
        stream_download(options, fd, progress, &written, &transfer_error, resume_from,
                        bandwidth_limit, &stream_speed);
    sync_file(fd);
    close_file(fd);

    if (outcome == TransferOutcome::Completed) {
        if (known_size && written != probe.content_length) {
            result.status = DownloadResult::Status::Failed;
            result.error = "incomplete download";
            result.path = ws.part_path;
            result.size = written;
            return result;
        }
        std::string final_path;
        std::string move_error;
        if (!finalize_part_file(ws.part_path, out_dir, filename, overwrite, &final_path,
                                &move_error)) {
            result.status = DownloadResult::Status::Failed;
            result.error = move_error;
            return result;
        }
        remove_tree(ws.dir);
        result.status = DownloadResult::Status::Completed;
        result.path = final_path;
        result.size = written > 0 ? written : probe.content_length;
        return result;
    }

    result.path = ws.part_path;
    result.size = probe.content_length;
    if (outcome == TransferOutcome::Paused) {
        result.status = DownloadResult::Status::Paused;
        return result;
    }
    result.status = DownloadResult::Status::Failed;
    result.error = transfer_error.empty() ? "download failed" : transfer_error;
    return result;
}

// ---------------------------------------------------------------------------
// HLS: fetch the playlist, then every segment, concatenated into one file.
// Resume continues at the first segment that was not fully written.
// ---------------------------------------------------------------------------
DownloadResult run_hls(const HttpOptions& base_options, const ProbeResult& probe,
                       const std::string& filename, const std::string& out_dir,
                       const DownloadWorkspace& ws, bool allow_resume, bool overwrite,
                       const std::shared_ptr<BandwidthLimit>& bandwidth_limit,
                       const std::function<bool(const ProgressInfo&)>& on_progress) {
    (void)probe;
    DownloadResult result;
    result.workspace_dir = ws.dir;

    HttpOptions options = base_options;
    std::string text;
    std::string effective_url;
    std::string fetch_error;
    if (!fetch_text(options, &text, &effective_url, &fetch_error)) {
        result.status = DownloadResult::Status::Failed;
        result.error = fetch_error.empty() ? "cannot read the HLS playlist" : fetch_error;
        return result;
    }
    if (!effective_url.empty() && !same_url_origin(options.url, effective_url)) {
        options.headers.clear();
        options.referer.clear();
        options.post_data.clear();
    }

    HlsPlaylist playlist;
    std::string parse_error;
    if (!parse_hls_playlist(text, &playlist, &parse_error)) {
        result.status = DownloadResult::Status::Failed;
        result.error = parse_error;
        return result;
    }
    if (playlist.master) {
        const auto best =
            std::max_element(playlist.variants.begin(), playlist.variants.end(),
                             [](const auto& a, const auto& b) { return a.first < b.first; });
        options.url = resolve_playlist_uri(effective_url, best->second);
        if (!is_supported_download_url(options.url)) {
            result.status = DownloadResult::Status::Failed;
            result.error = "the HLS master playlist references an unsupported URL scheme";
            return result;
        }
        text.clear();
        if (!fetch_text(options, &text, &effective_url, &fetch_error)) {
            result.status = DownloadResult::Status::Failed;
            result.error = fetch_error;
            return result;
        }
        if (!effective_url.empty() && !same_url_origin(options.url, effective_url)) {
            options.headers.clear();
            options.referer.clear();
            options.post_data.clear();
        }
        if (!parse_hls_playlist(text, &playlist, &parse_error)) {
            result.status = DownloadResult::Status::Failed;
            result.error = parse_error;
            return result;
        }
        if (playlist.master) {
            result.status = DownloadResult::Status::Failed;
            result.error = "nested HLS master playlists are not supported";
            return result;
        }
    }
    if (playlist.encrypted) {
        result.status = DownloadResult::Status::Failed;
        result.error = "encrypted HLS playlists are not supported yet";
        return result;
    }
    if (playlist.segments.empty()) {
        result.status = DownloadResult::Status::Failed;
        result.error = "the HLS playlist has no segments";
        return result;
    }

    // Segment URIs are remote input. A file:// entry would make libcurl copy a
    // local file into the output, so every URI has to pass the same allowlist
    // as a user-supplied URL before the part file is even created.
    const auto known_scheme = [&](const std::string& uri) {
        return is_supported_download_url(resolve_playlist_uri(effective_url, uri));
    };
    const bool bad_init = !playlist.init_segment.empty() && !known_scheme(playlist.init_segment);
    const bool bad_segment = std::any_of(
        playlist.segments.begin(), playlist.segments.end(),
        [&](const std::string& uri) { return !known_scheme(uri); });
    if (bad_init || bad_segment) {
        result.status = DownloadResult::Status::Failed;
        result.error = "the HLS playlist references an unsupported URL scheme";
        return result;
    }

    // The progress file holds the index of the next segment to fetch; the part
    // file holds the concatenated segments.
    const std::string progress_path =
        path_to_utf8(path_from_utf8(ws.dir) / "hls_progress");
    std::error_code ec;
    uint64_t next_index = 0;
    if (allow_resume && std::filesystem::exists(path_from_utf8(ws.part_path), ec) &&
        std::filesystem::exists(path_from_utf8(progress_path), ec)) {
        std::ifstream in(path_from_utf8(progress_path));
        uint64_t stored = 0;
        if (in >> stored) next_index = std::min<uint64_t>(stored, playlist.segments.size());
    }

    const unsigned flags = FileWrite | FileCreate |
                           (next_index > 0 ? FileAppend : FileTruncate);
    const int fd = open_file(ws.part_path, flags);
    if (fd < 0) {
        result.status = DownloadResult::Status::Failed;
        result.error = "cannot create '" + ws.part_path + "': " + errno_string(errno);
        return result;
    }

    ProgressInfo info;
    info.total = 0;  // byte total unknown; percent follows the segment index
    uint64_t bytes_done = 0;
    if (next_index > 0) {
        const uint64_t have = std::filesystem::file_size(path_from_utf8(ws.part_path), ec);
        if (!ec) bytes_done = have;
    }
    info.done = bytes_done;
    info.percent = static_cast<int>(percent_of(next_index, playlist.segments.size()));

    const auto save_progress = [&](uint64_t index) {
        std::ofstream out(path_from_utf8(progress_path), std::ios::trunc);
        out << index;
    };

    // Speed across segments. A per-request speed would drop to zero at every
    // segment boundary, so the aggregate byte count is sampled here instead,
    // over the same 200 ms window and smoothing the other transfer paths use.
    using Clock = std::chrono::steady_clock;
    const Clock::time_point started = Clock::now();
    auto elapsed_ms = [&started](Clock::time_point t) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(t - started).count();
    };
    constexpr int64_t kSegmentSpeedSampleMs = 200;
    int64_t last_sample_ms = 0;
    uint64_t last_sampled_bytes = bytes_done;
    double aggregate_speed = 0.0;
    const auto sample_speed = [&](uint64_t done) {
        const int64_t now = elapsed_ms(Clock::now());
        const int64_t dt_ms = now - last_sample_ms;
        if (dt_ms < kSegmentSpeedSampleMs) return;
        const double dt = static_cast<double>(dt_ms) / 1000.0;
        const double inst = static_cast<double>(done - last_sampled_bytes) / dt;
        aggregate_speed = (aggregate_speed == 0.0) ? inst : (0.6 * aggregate_speed + 0.4 * inst);
        last_sampled_bytes = done;
        last_sample_ms = now;
    };
    const auto report = [&]() -> bool {
        info.speed_bps = aggregate_speed;
        return on_progress ? on_progress(info) : true;
    };

    std::string transfer_error;
    bool paused = false;
    bool failed = false;
    const auto fetch_one = [&](const std::string& uri) -> bool {
        HttpOptions segment_options = options;
        segment_options.url = resolve_playlist_uri(effective_url, uri);
        uint64_t written_here = 0;
        const TransferOutcome outcome =
            stream_download(segment_options, fd,
                            [&](uint64_t) -> bool {
                                sample_speed(bytes_done + written_here);
                                return report();
                            },
                            &written_here, &transfer_error, 0, bandwidth_limit);
        bytes_done += written_here;
        info.done = bytes_done;
        // The last sample lands before the final byte, so the next segment
        // starts measuring from the true total rather than trailing behind.
        sample_speed(bytes_done);
        if (outcome == TransferOutcome::Completed) return true;
        if (outcome == TransferOutcome::Paused) {
            paused = true;
            return false;
        }
        failed = true;
        return false;
    };

    // Report the starting state first so a pause request stops before any
    // network I/O.
    if (!report()) paused = true;

    if (!paused && !failed && next_index == 0 && !playlist.init_segment.empty()) {
        if (!fetch_one(playlist.init_segment)) {
            // The init segment is not counted as a media segment.
        }
    }
    for (uint64_t i = next_index; !paused && !failed && i < playlist.segments.size(); ++i) {
        if (!fetch_one(playlist.segments[i])) break;
        next_index = i + 1;
        info.percent = static_cast<int>(percent_of(next_index, playlist.segments.size()));
        save_progress(next_index);
        if (!report()) {
            paused = true;
            break;
        }
    }
    // The transfer is over: report the final byte count with no rate, so the
    // Bandwidth column does not keep showing a stale speed until finished().
    info.speed_bps = 0.0;
    if (on_progress) on_progress(info);
    sync_file(fd);
    close_file(fd);
    save_progress(next_index);
    if (failed) {
        result.status = DownloadResult::Status::Failed;
        result.error = transfer_error.empty() ? "a segment could not be downloaded"
                                              : transfer_error;
        result.path = ws.part_path;
        return result;
    }
    if (paused || next_index < playlist.segments.size()) {
        result.status = DownloadResult::Status::Paused;
        result.path = ws.part_path;
        return result;
    }

    std::string final_path;
    std::string move_error;
    if (!finalize_part_file(ws.part_path, out_dir, filename, overwrite, &final_path,
                            &move_error)) {
        result.status = DownloadResult::Status::Failed;
        result.error = move_error;
        result.path = ws.part_path;
        return result;
    }
    remove_tree(ws.dir);
    result.status = DownloadResult::Status::Completed;
    result.path = final_path;
    result.size = std::filesystem::file_size(path_from_utf8(final_path), ec);
    return result;
}

}  // namespace engine_internal
}  // namespace swiftdm
