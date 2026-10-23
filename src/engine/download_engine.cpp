#include "engine/download_engine.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include "core/log.h"
#include "core/util.h"
#include "core/file_io.h"
#include "engine/categories.h"
#include "engine/hls.h"
#include "engine/segment_plan.h"

namespace swiftdm {
namespace {

int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

std::string pick_filename(const ProbeResult& probe, const std::string& url) {
    if (!probe.filename_hint.empty()) return probe.filename_hint;
    std::string name = file_name_from_url(probe.effective_url);
    if (name.empty()) name = file_name_from_url(url);
    if (name.empty()) name = "download.bin";
    return name;
}

bool move_file(const std::string& from, const std::string& to, std::string* error) {
    std::error_code ec;
    std::filesystem::rename(path_from_utf8(from), path_from_utf8(to), ec);
    if (!ec) return true;
    // Cross-device move fallback.
    std::error_code copy_ec;
    std::filesystem::copy_file(path_from_utf8(from), path_from_utf8(to),
                               std::filesystem::copy_options::overwrite_existing,
                               copy_ec);
    if (copy_ec) {
        if (error) *error = "cannot move to '" + to + "': " + copy_ec.message();
        return false;
    }
    std::filesystem::remove(path_from_utf8(from), ec);
    return true;
}

std::string lower_copy(const std::string& text) {
    std::string out(text);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

// HLS playlists are fetched as .ts by convention; keep an explicit caller
// filename untouched unless it still carries the playlist extension.
std::string hls_output_filename(const std::string& filename) {
    const size_t dot = filename.rfind('.');
    if (dot == std::string::npos) return filename + ".ts";
    const std::string ext = lower_copy(filename.substr(dot));
    if (ext == ".m3u8" || ext == ".m3u") return filename.substr(0, dot) + ".ts";
    return filename;
}

// ---------------------------------------------------------------------------
// FTP: single connection, REST-based resume (spec 04-protocols.md §2).
// ---------------------------------------------------------------------------
DownloadResult run_ftp(const HttpOptions& options, const ProbeResult& probe,
                       const std::string& filename, const std::string& out_dir,
                       const DownloadWorkspace& ws, bool allow_resume,
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
    auto progress = [&](uint64_t done) -> bool {
        info.done = done;
        info.percent = percent_of(done, probe.content_length);
        return on_progress ? on_progress(info) : true;
    };
    uint64_t written = 0;
    std::string transfer_error;
    const TransferOutcome outcome =
        stream_download(options, fd, progress, &written, &transfer_error, resume_from,
                        bandwidth_limit);
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
        const std::string final_path =
            unique_path(path_to_utf8(path_from_utf8(out_dir) / path_from_utf8(filename)));
        std::string move_error;
        if (!move_file(ws.part_path, final_path, &move_error)) {
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
                       const DownloadWorkspace& ws, bool allow_resume,
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
        text.clear();
        if (!fetch_text(options, &text, &effective_url, &fetch_error)) {
            result.status = DownloadResult::Status::Failed;
            result.error = fetch_error;
            return result;
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

    std::string transfer_error;
    bool paused = false;
    bool failed = false;
    const auto fetch_one = [&](const std::string& uri) -> bool {
        HttpOptions segment_options = base_options;
        segment_options.url = resolve_playlist_uri(effective_url, uri);
        uint64_t written_here = 0;
        const TransferOutcome outcome =
            stream_download(segment_options, fd,
                            [&](uint64_t) -> bool {
                                return on_progress ? on_progress(info) : true;
                            },
                            &written_here, &transfer_error, 0, bandwidth_limit);
        bytes_done += written_here;
        info.done = bytes_done;
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
    if (on_progress && !on_progress(info)) paused = true;

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
        if (on_progress && !on_progress(info)) {
            paused = true;
            break;
        }
    }
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

    const std::string final_path =
        unique_path(path_to_utf8(path_from_utf8(out_dir) / path_from_utf8(filename)));
    std::string move_error;
    if (!move_file(ws.part_path, final_path, &move_error)) {
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

}  // namespace

DownloadWorkspace make_workspace(const Settings& settings, const std::string& url,
                                 const std::string& filename) {
    DownloadWorkspace ws;
    const std::filesystem::path dir =
        path_from_utf8(settings.temp_dir) /
        ("download-" + hex_hash64(fnv1a_hash(url + "\n" + filename)));
    ws.dir = path_to_utf8(dir);
    // The temporary file is a path component of its own, so it needs the same
    // length limit as the final name (a long name would fail with
    // "File name too long").
    ws.part_path =
        path_to_utf8(dir / path_from_utf8(limit_filename_length(filename + ".part", kMaxFileNameBytes)));
    ws.state_path = path_to_utf8(dir / "segments.json");
    return ws;
}

std::string unique_path(const std::string& path) {
    std::error_code ec;
    const std::filesystem::path original = path_from_utf8(path);
    // Guard against a caller that passes a name which was not sanitized.
    const std::filesystem::path p =
        original.parent_path() /
        path_from_utf8(limit_filename_length(path_to_utf8(original.filename()), kMaxFileNameBytes));
    if (!std::filesystem::exists(p, ec)) return path_to_utf8(p);
    const std::string stem = path_to_utf8(p.stem());
    const std::string ext = path_to_utf8(p.extension());
    for (int i = 1; i < 10000; ++i) {
        const std::string tag = " (" + std::to_string(i) + ")";
        // The " (N)" variant has to stay within the limit as well, so the
        // stem is shortened instead of the tag.
        size_t budget = 0;
        if (kMaxFileNameBytes > tag.size() + ext.size()) {
            budget = kMaxFileNameBytes - tag.size() - ext.size();
        }
        std::string trimmed = stem;
        if (trimmed.size() > budget) {
            trimmed = std::string(utf8_truncate(trimmed, budget));
        }
        const std::filesystem::path candidate = p.parent_path() / path_from_utf8(trimmed + tag + ext);
        if (!std::filesystem::exists(candidate, ec)) return path_to_utf8(candidate);
    }
    return path_to_utf8(p);
}

DownloadEngine::DownloadEngine(Settings settings) : settings_(std::move(settings)) {}

DownloadResult DownloadEngine::run(
    const DownloadRequest& request,
    const std::function<bool(const ProgressInfo&)>& on_progress) {
    DownloadResult result;

    const int connections =
        request.connections > 0 ? request.connections : settings_.max_connections;
    // The toggle is authoritative: off means unlimited and does not fall back
    // to the global setting. Callers (CLI/GUI) seed the request from settings
    // when the user has not overridden them.
    const int limit_kbps = request.bandwidth_limit_enabled
                               ? std::max(0, request.bandwidth_limit_kbps)
                               : 0;
    const int64_t limit_bytes = static_cast<int64_t>(limit_kbps) * 1024;
    const auto bandwidth_limit = request.live_bandwidth_limit
                                     ? request.live_bandwidth_limit
                                     : std::make_shared<BandwidthLimit>(limit_bytes);

    HttpOptions options;
    options.url = request.url;
    options.headers = request.headers;
    options.cookies = request.cookies;
    options.user_agent = !request.user_agent.empty() ? request.user_agent
                                                     : settings_.default_agent;
    options.referer = request.referer;
    options.post_data = request.post_data;
    if (settings_.proxy.type != "none" && !settings_.proxy.address.empty()) {
        options.proxy_type = settings_.proxy.type;
        options.proxy_address = settings_.proxy.address;
        options.proxy_port = settings_.proxy.port;
        options.proxy_user = settings_.proxy.user;
        options.proxy_password = settings_.proxy.password;
    }

    const ProbeResult probe = request.probe && request.probe->ok
                                  ? *request.probe
                                  : probe_url(options);
    if (!probe.ok) {
        result.status = DownloadResult::Status::Failed;
        result.error = probe.error.empty() ? "probe failed" : probe.error;
        return result;
    }
    // A resolved link (Drive viewer links and virus-scan notices) names the URL
    // that actually serves the file; every transfer below must use it.
    if (!probe.download_url.empty()) options.url = probe.download_url;

    std::string filename = request.filename.empty() ? pick_filename(probe, request.url)
                                                    : sanitize_filename(request.filename);
    // The name becomes a path component on disk, so it has to fit the limit of
    // the filesystem no matter where it came from.
    filename = limit_filename_length(sanitize_filename(filename), kMaxFileNameBytes);
    if (filename.empty()) filename = "download.bin";

    std::string out_dir =
        request.output_dir.empty() ? settings_.download_dir : request.output_dir;
    if (settings_.category_folders && request.apply_category_folder) {
        out_dir = path_to_utf8(path_from_utf8(out_dir) / path_from_utf8(category_folder_for(filename)));
    }

    std::string dir_error;
    if (!ensure_directory(out_dir, &dir_error) ||
        !ensure_directory(settings_.temp_dir, &dir_error)) {
        result.status = DownloadResult::Status::Failed;
        result.error = dir_error;
        return result;
    }

    const std::string workspace_key =
        probe.effective_url.empty() ? request.url : probe.effective_url;
    const DownloadWorkspace ws = make_workspace(settings_, workspace_key, filename);
    if (!ensure_directory(ws.dir, &dir_error)) {
        result.status = DownloadResult::Status::Failed;
        result.error = dir_error;
        return result;
    }
    result.workspace_dir = ws.dir;

    // ------------------------------------------------------------------
    // FTP and HLS take dedicated paths (04-protocols.md §2, HLS spec note).
    // ------------------------------------------------------------------
    if (is_ftp_url(workspace_key)) {
        return run_ftp(options, probe, filename, out_dir, ws, request.resume, bandwidth_limit,
                       on_progress);
    }
    if (is_hls_url(workspace_key, probe.mime_type)) {
        return run_hls(options, probe,
                       limit_filename_length(hls_output_filename(filename), kMaxFileNameBytes),
                       out_dir, ws, request.resume, bandwidth_limit, on_progress);
    }

    const bool can_resume = probe.accept_ranges && request.post_data.empty();
    const bool known_size = probe.content_length > 0;

    // ------------------------------------------------------------------
    // Unknown size: sequential streaming, no resume (spec 03-engine.md).
    // ------------------------------------------------------------------
    if (!known_size) {
        const int fd = open_file(ws.part_path, FileWrite | FileCreate | FileTruncate);
        if (fd < 0) {
            result.status = DownloadResult::Status::Failed;
            result.error = "cannot create '" + ws.part_path + "': " + errno_string(errno);
            return result;
        }
        ProgressInfo info;
        auto progress = [&](uint64_t done) -> bool {
            info.done = done;
            info.percent = 0;
            return on_progress ? on_progress(info) : true;
        };
        uint64_t written = 0;
        std::string transfer_error;
        const TransferOutcome outcome =
            stream_download(options, fd, progress, &written, &transfer_error, 0,
                            bandwidth_limit);
        sync_file(fd);
        close_file(fd);

        if (outcome == TransferOutcome::Completed) {
            std::string move_error;
            const std::string final_path =
                unique_path(path_to_utf8(path_from_utf8(out_dir) / path_from_utf8(filename)));
            if (!move_file(ws.part_path, final_path, &move_error)) {
                result.status = DownloadResult::Status::Failed;
                result.error = move_error;
                return result;
            }
            remove_tree(ws.dir);
            result.status = DownloadResult::Status::Completed;
            result.path = final_path;
            result.size = written;
            return result;
        }

        std::error_code ec;
        std::filesystem::remove(path_from_utf8(ws.part_path), ec);
        if (outcome == TransferOutcome::Paused) {
            result.status = DownloadResult::Status::Paused;
            return result;
        }
        result.status = DownloadResult::Status::Failed;
        result.error = transfer_error.empty() ? "download failed" : transfer_error;
        return result;
    }

    // ------------------------------------------------------------------
    // Known size: segmented transfer with resume state.
    // ------------------------------------------------------------------
    TransferState state;
    bool have_state = false;
    if (request.resume && can_resume) {
        TransferState loaded;
        if (load_transfer_state(ws.state_path, loaded, nullptr) && !loaded.segments.empty()) {
            const bool same_size = loaded.file_size == probe.content_length;
            const bool same_etag = probe.etag.empty() || loaded.etag.empty() ||
                                   loaded.etag == probe.etag;
            const bool same_lm = probe.last_modified.empty() || loaded.last_modified.empty() ||
                                 loaded.last_modified == probe.last_modified;
            if (same_size && same_etag && same_lm) {
                state = std::move(loaded);
                have_state = true;
            } else {
                log_warn("resume state does not match server file; restarting download");
            }
        }
    }

    if (!have_state) {
        state = TransferState{};
        state.url = workspace_key;
        state.file_size = probe.content_length;
        state.etag = probe.etag;
        state.last_modified = probe.last_modified;
        const int planned = can_resume ? connections : 1;
        state.segments = plan_segments(state.file_size, planned);
    }

    const int fd = open_file(ws.part_path, FileRead | FileWrite | FileCreate);
    if (fd < 0) {
        result.status = DownloadResult::Status::Failed;
        result.error = "cannot create '" + ws.part_path + "': " + errno_string(errno);
        return result;
    }
    if (resize_file(fd, state.file_size) != 0) {
        close_file(fd);
        result.status = DownloadResult::Status::Failed;
        result.error = "cannot size '" + ws.part_path + "': " + errno_string(errno);
        return result;
    }

    ProgressInfo info;
    int64_t last_save = now_ms();
    auto progress = [&](uint64_t done, uint64_t total, double speed) -> bool {
        info.done = done;
        info.total = total;
        info.speed_bps = speed;
        info.percent = percent_of(done, total);
        info.segments = state.segments;
        const int64_t t = now_ms();
        if (request.resume && t - last_save >= 2000) {
            last_save = t;
            save_transfer_state(ws.state_path, state, nullptr);
        }
        return on_progress ? on_progress(info) : true;
    };

    SegmentDownloader downloader(options, state, fd);
    downloader.set_one_by_one(settings_.connections_at_once != 0);
    downloader.set_bandwidth_limit(bandwidth_limit);
    std::string transfer_error;
    TransferOutcome outcome = downloader.run(progress, &transfer_error);

    // Hosts with a per-IP connection limit refuse parallel ranges (pixeldrain's
    // free tier answers 403 max_concurrent_downloads, others drop the extra
    // connections). Finish the remaining ranges on a single connection once
    // before reporting the failure.
    if (outcome == TransferOutcome::Failed && connections > 1 && state.segments.size() > 1 &&
        http_error_status(transfer_error) == 403) {
        log_warn("parallel ranges refused with 403; retrying with a single connection");
        std::this_thread::sleep_for(std::chrono::seconds(1));
        downloader.set_one_by_one(true);
        transfer_error.clear();
        outcome = downloader.run(progress, &transfer_error);
    }
    sync_file(fd);
    close_file(fd);

    switch (outcome) {
        case TransferOutcome::Completed: {
            if (state.done_bytes() != state.file_size) {
                save_transfer_state(ws.state_path, state, nullptr);
                result.status = DownloadResult::Status::Failed;
                result.error = "incomplete download";
                return result;
            }
            const std::string final_path =
                unique_path(path_to_utf8(path_from_utf8(out_dir) / path_from_utf8(filename)));
            std::string move_error;
            if (!move_file(ws.part_path, final_path, &move_error)) {
                save_transfer_state(ws.state_path, state, nullptr);
                result.status = DownloadResult::Status::Failed;
                result.error = move_error;
                return result;
            }
            remove_transfer_state(ws.state_path);
            remove_tree(ws.dir);
            result.status = DownloadResult::Status::Completed;
            result.path = final_path;
            result.size = state.file_size;
            return result;
        }
        case TransferOutcome::Paused:
            if (request.resume) save_transfer_state(ws.state_path, state, nullptr);
            result.status = DownloadResult::Status::Paused;
            result.path = ws.part_path;
            result.size = state.file_size;
            return result;
        case TransferOutcome::Failed:
        default:
            if (request.resume) save_transfer_state(ws.state_path, state, nullptr);
            result.status = DownloadResult::Status::Failed;
            result.path = ws.part_path;
            result.size = state.file_size;
            result.error = transfer_error.empty() ? "download failed" : transfer_error;
            return result;
    }
}

}  // namespace swiftdm
