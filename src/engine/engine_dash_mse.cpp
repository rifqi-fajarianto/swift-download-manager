#include "engine/engine_internal.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include "core/file_io.h"
#include "core/util.h"
#include "engine/dash.h"
#include "engine/mp4_boxes.h"
#include "net/http.h"

namespace swiftdm {
namespace engine_internal {
namespace {

// Downloads a resolved segment list into the part file, appending in order.
// Progress is reported by segment index (byte totals are unknown until the
// end), matching the HLS path. `verify`, when present, runs on the finished
// part file before it is moved to its final name.
DownloadResult run_segmented(
    const HttpOptions& base_options,
    const std::vector<std::string>& urls,
    const std::string& filename,
    const std::string& out_dir,
    const DownloadWorkspace& ws,
    bool allow_resume,
    bool overwrite,
    const std::shared_ptr<BandwidthLimit>& bandwidth_limit,
    const std::function<bool(const ProgressInfo&)>& on_progress,
    const std::string& progress_name,
    const std::function<bool(const std::string&, std::string*)>& verify) {
    DownloadResult result;
    result.workspace_dir = ws.dir;
    if (urls.empty()) {
        result.status = DownloadResult::Status::Failed;
        result.error = "no segments to download";
        return result;
    }
    for (const auto& url : urls) {
        if (!is_supported_download_url(url)) {
            result.status = DownloadResult::Status::Failed;
            result.error = "the media stream references an unsupported URL scheme: " + url;
            return result;
        }
    }

    std::error_code ec;
    const std::string progress_path =
        path_to_utf8(path_from_utf8(ws.dir) / progress_name);
    uint64_t next_index = 0;
    if (allow_resume && std::filesystem::exists(path_from_utf8(ws.part_path), ec) &&
        std::filesystem::exists(progress_path, ec)) {
        std::ifstream in(progress_path);
        uint64_t stored = 0;
        if (in >> stored) next_index = std::min<uint64_t>(stored, urls.size());
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
    info.total = 0;
    uint64_t bytes_done = 0;
    if (next_index > 0) {
        const uint64_t have =
            std::filesystem::file_size(path_from_utf8(ws.part_path), ec);
        if (!ec) bytes_done = have;
    }
    info.done = bytes_done;
    info.percent = static_cast<int>(percent_of(next_index, urls.size()));

    const auto save_progress = [&](uint64_t index) {
        std::ofstream out(progress_path, std::ios::trunc);
        out << index;
    };

    using Clock = std::chrono::steady_clock;
    const Clock::time_point started = Clock::now();
    auto elapsed_ms = [&started](Clock::time_point t) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(t - started)
            .count();
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
        const double inst =
            static_cast<double>(done - last_sampled_bytes) / dt;
        aggregate_speed = (aggregate_speed == 0.0)
                              ? inst
                              : (0.6 * aggregate_speed + 0.4 * inst);
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
    const auto fetch_one = [&](const std::string& url) -> bool {
        HttpOptions segment_options = base_options;
        segment_options.url = url;
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
        sample_speed(bytes_done);
        if (outcome == TransferOutcome::Completed) return true;
        if (outcome == TransferOutcome::Paused) {
            paused = true;
            return false;
        }
        failed = true;
        return false;
    };

    if (!report()) paused = true;
    for (uint64_t i = next_index; !paused && !failed && i < urls.size(); ++i) {
        if (!fetch_one(urls[i])) break;
        next_index = i + 1;
        info.percent = static_cast<int>(percent_of(next_index, urls.size()));
        save_progress(next_index);
        if (!report()) {
            paused = true;
            break;
        }
    }

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
    if (paused || next_index < urls.size()) {
        result.status = DownloadResult::Status::Paused;
        result.path = ws.part_path;
        return result;
    }

    if (verify) {
        std::string verify_error;
        if (!verify(ws.part_path, &verify_error)) {
            std::filesystem::remove(path_from_utf8(ws.part_path), ec);
            result.status = DownloadResult::Status::Failed;
            result.error = verify_error;
            result.path = ws.part_path;
            return result;
        }
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

uint32_t read_u32_at(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

// Walks the part file's top-level boxes and checks that every `moof` carries
// an `mfhd` sequence number one higher than the previous and a `tfdt` base
// decode time that does not move backwards (docs/spec/08-media-download.md
// §6.2). A gap means the user seeked or entries were dropped; the assembled
// file would be broken, so the download fails instead of writing it.
bool verify_mse_continuity(const std::string& part_path, std::string* error) {
    std::ifstream in(path_from_utf8(part_path), std::ios::binary);
    if (!in) return true;  // cannot read back: do not invent a failure

    std::vector<uint8_t> header(8);
    bool first = true;
    uint32_t prev_seq = 0;
    uint64_t prev_bdt = 0;
    while (in.read(reinterpret_cast<char*>(header.data()), 8)) {
        const uint32_t size = read_u32_at(header.data());
        const std::string type(reinterpret_cast<const char*>(header.data() + 4), 4);
        if (type == "moof") {
            in.seekg(-8, std::ios::cur);
            std::vector<uint8_t> box(size);
            if (!in.read(reinterpret_cast<char*>(box.data()),
                         static_cast<std::streamsize>(size))) {
                return true;  // truncated file: the transfer check already failed
            }
            const uint32_t seq = moof_sequence_number(box.data(), box.size());
            const uint64_t bdt = moof_base_decode_time(box.data(), box.size());
            if (!first) {
                if (seq != prev_seq + 1) {
                    *error = "the recorded stream has a gap; play the video from the start and try again";
                    return false;
                }
                if (bdt < prev_bdt) {
                    *error = "the recorded stream has a gap; play the video from the start and try again";
                    return false;
                }
            }
            prev_seq = seq;
            prev_bdt = bdt;
            first = false;
        } else {
            if (size < 8) break;
            in.seekg(static_cast<std::streamsize>(size - 8), std::ios::cur);
        }
    }
    return true;
}

}  // namespace

DownloadResult run_dash(const DownloadRequest& request, const HttpOptions& options_in,
                        const std::string& filename, const std::string& out_dir,
                        const DownloadWorkspace& ws, bool allow_resume, bool overwrite,
                        const std::shared_ptr<BandwidthLimit>& bandwidth_limit,
                        const std::function<bool(const ProgressInfo&)>& on_progress) {
    DownloadResult result;
    result.workspace_dir = ws.dir;

    HttpOptions options = options_in;
    std::string text;
    std::string effective_url;
    std::string fetch_error;
    if (!fetch_text(options, &text, &effective_url, &fetch_error)) {
        result.status = DownloadResult::Status::Failed;
        result.error = fetch_error.empty() ? "cannot read DASH manifest" : fetch_error;
        return result;
    }
    if (!effective_url.empty() && !same_url_origin(options.url, effective_url)) {
        options.headers.clear();
        options.referer.clear();
        options.post_data.clear();
    }

    DashManifest manifest;
    std::string parse_error;
    if (!parse_dash_manifest(text, effective_url, &manifest, &parse_error)) {
        result.status = DownloadResult::Status::Failed;
        result.error = parse_error;
        return result;
    }

    // Pick the requested rendition, else the best video, else the best audio.
    const ui::MediaVariant* chosen = nullptr;
    if (!request.variant_id.empty()) {
        for (const auto& v : manifest.video_representations) {
            if (v.representation_id == request.variant_id) {
                chosen = &v;
                break;
            }
        }
        if (!chosen) {
            for (const auto& v : manifest.audio_representations) {
                if (v.representation_id == request.variant_id) {
                    chosen = &v;
                    break;
                }
            }
        }
    }
    if (!chosen && !manifest.video_representations.empty()) {
        chosen = &manifest.video_representations.front();
    }
    if (!chosen && !manifest.audio_representations.empty()) {
        chosen = &manifest.audio_representations.front();
    }
    if (!chosen) {
        result.status = DownloadResult::Status::Failed;
        result.error = "no DASH representation found";
        return result;
    }

    std::vector<std::string> urls;
    if (!chosen->init_url.empty()) urls.push_back(chosen->init_url);
    for (const auto& segment : chosen->segment_urls) urls.push_back(segment);
    if (urls.empty()) {
        result.status = DownloadResult::Status::Failed;
        result.error = "the DASH representation has no segments";
        return result;
    }

    return run_segmented(options, urls, filename, out_dir, ws, allow_resume,
                         overwrite, bandwidth_limit, on_progress, "dash_progress",
                         nullptr);
}

DownloadResult run_mse(const DownloadRequest& request, const HttpOptions& options_in,
                       const std::string& filename, const std::string& out_dir,
                       const DownloadWorkspace& ws, bool allow_resume, bool overwrite,
                       const std::shared_ptr<BandwidthLimit>& bandwidth_limit,
                       const std::function<bool(const ProgressInfo&)>& on_progress) {
    DownloadResult result;
    result.workspace_dir = ws.dir;
    if (request.segments.empty()) {
        result.status = DownloadResult::Status::Failed;
        result.error = "no recorded media segments";
        return result;
    }

    std::vector<const ui::MediaSegment*> ordered;
    ordered.reserve(request.segments.size());
    for (const auto& segment : request.segments) ordered.push_back(&segment);
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const ui::MediaSegment* a, const ui::MediaSegment* b) {
                         return a->order < b->order;
                     });

    std::vector<std::string> urls;
    urls.reserve(ordered.size());
    for (const ui::MediaSegment* segment : ordered) urls.push_back(segment->url);

    return run_segmented(options_in, urls, filename, out_dir, ws, allow_resume,
                         overwrite, bandwidth_limit, on_progress, "mse_progress",
                         [](const std::string& part_path, std::string* error) {
                             return verify_mse_continuity(part_path, error);
                         });
}

}  // namespace engine_internal
}  // namespace swiftdm
