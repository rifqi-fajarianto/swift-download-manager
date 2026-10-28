#include "engine/download_engine.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "core/file_io.h"
#include "core/log.h"
#include "core/util.h"
#include "core/win_headers.h"
#include "engine/categories.h"
#include "engine/hls.h"
#include "engine/segment_plan.h"

#ifndef _WIN32
#include <unistd.h>
#endif

namespace swiftdm {

// Defined below, after the naming helpers; the transfer paths inside the
// anonymous namespace finish their part file through it.
bool move_file_unique(const std::string& from, const std::string& dir,
                      const std::string& filename, std::string* final_path,
                      std::string* error);

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

enum class ClaimResult { Claimed, Exists, Failed };

// Claims `to` for `from` without ever replacing an existing file. A hard link
// claims the destination atomically and fails when the name is taken; a
// cross-device move falls back to creating the destination exclusively and
// copying into it, which keeps the same no-overwrite rule.
ClaimResult claim_path(const std::string& from, const std::string& to, std::string* error) {
#ifdef _WIN32
    const std::filesystem::path source = path_from_utf8(from);
    const std::filesystem::path destination = path_from_utf8(to);
    if (CreateHardLinkW(destination.c_str(), source.c_str(), nullptr)) {
        // The destination is now in place; leaving the source behind only
        // wastes space in the workspace that is about to be deleted.
        std::error_code ignored;
        std::filesystem::remove(source, ignored);
        return ClaimResult::Claimed;
    }
    const DWORD link_error = GetLastError();
    if (link_error == ERROR_ALREADY_EXISTS || link_error == ERROR_FILE_EXISTS) {
        return ClaimResult::Exists;
    }
    // A volume boundary, a filesystem without hard links (FAT32) or a protected
    // destination falls through to the exclusive copy, which reports its own
    // error if the destination really cannot be written.
    if (link_error != ERROR_NOT_SAME_DEVICE && link_error != ERROR_NOT_SUPPORTED &&
        link_error != ERROR_INVALID_FUNCTION && link_error != ERROR_ACCESS_DENIED) {
        if (error) {
            *error = "cannot move to '" + to + "': " +
                     std::system_category().message(static_cast<int>(link_error));
        }
        return ClaimResult::Failed;
    }
#else
    if (::link(from.c_str(), to.c_str()) == 0) {
        // See above: the leftover source is cheaper than a failed download.
        (void)::unlink(from.c_str());
        return ClaimResult::Claimed;
    }
    if (errno == EEXIST) return ClaimResult::Exists;
    const int link_errno = errno;
    if (link_errno != EXDEV && link_errno != EPERM && link_errno != EOPNOTSUPP &&
        link_errno != ENOSYS) {
        if (error) {
            *error = "cannot move to '" + to + "': " + std::strerror(link_errno);
        }
        return ClaimResult::Failed;
    }
#endif

    // Different filesystem (or a filesystem without hard links): create the
    // destination exclusively, then copy.
    const int out_fd = open_file(to, FileWrite | FileCreate | FileExclusive);
    if (out_fd < 0) {
        if (errno == EEXIST) return ClaimResult::Exists;
        if (error) *error = "cannot create '" + to + "': " + std::strerror(errno);
        return ClaimResult::Failed;
    }
    const int in_fd = open_file(from, FileRead);
    if (in_fd < 0) {
        if (error) *error = "cannot read '" + from + "': " + std::strerror(errno);
        close_file(out_fd);
        std::error_code ignored;
        std::filesystem::remove(path_from_utf8(to), ignored);
        return ClaimResult::Failed;
    }
    bool ok = true;
    std::vector<char> buffer(64 * 1024);
    while (true) {
        const std::ptrdiff_t got = read_file(in_fd, buffer.data(), buffer.size());
        if (got < 0) {
            if (error) *error = "cannot read '" + from + "': " + std::strerror(errno);
            ok = false;
            break;
        }
        if (got == 0) break;
        if (write_file(out_fd, buffer.data(), static_cast<size_t>(got)) < 0) {
            if (error) *error = "cannot write '" + to + "': " + std::strerror(errno);
            ok = false;
            break;
        }
    }
    close_file(in_fd);
    if (ok && sync_file(out_fd) != 0) {
        if (error) *error = "cannot sync '" + to + "': " + std::strerror(errno);
        ok = false;
    }
    if (close_file(out_fd) != 0 && ok) {
        if (error) *error = "cannot close '" + to + "': " + std::strerror(errno);
        ok = false;
    }
    std::error_code ignored;
    if (!ok) {
        std::filesystem::remove(path_from_utf8(to), ignored);
        return ClaimResult::Failed;
    }
    std::filesystem::remove(path_from_utf8(from), ignored);
    return ClaimResult::Claimed;
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
        std::string final_path;
        std::string move_error;
        if (!move_file_unique(ws.part_path, out_dir, filename, &final_path, &move_error)) {
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

    std::string final_path;
    std::string move_error;
    if (!move_file_unique(ws.part_path, out_dir, filename, &final_path, &move_error)) {
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

// Candidate `index` for a final name: index 0 is the plain name, then
// " (1)", " (2)", … The " (N)" variant has to stay within the path-component
// limit as well, so the stem is shortened instead of the tag.
std::filesystem::path candidate_path(const std::filesystem::path& dir,
                                     const std::string& filename, int index) {
    const std::string limited = limit_filename_length(filename, kMaxFileNameBytes);
    if (index == 0) return dir / path_from_utf8(limited);
    // The name may hold any UTF-8 text; counting and shortening it as UTF-8
    // keeps the component limit meaningful on Windows as well, where
    // path::string() would go through the ANSI code page.
    const std::filesystem::path base = path_from_utf8(limited);
    const std::string stem = path_to_utf8(base.stem());
    const std::string ext = path_to_utf8(base.extension());
    const std::string tag = " (" + std::to_string(index) + ")";
    size_t budget = 0;
    if (kMaxFileNameBytes > tag.size() + ext.size()) {
        budget = kMaxFileNameBytes - tag.size() - ext.size();
    }
    std::string trimmed = stem;
    if (trimmed.size() > budget) trimmed = std::string(utf8_truncate(trimmed, budget));
    return dir / path_from_utf8(trimmed + tag + ext);
}

std::string unique_path(const std::string& path) {
    std::error_code ec;
    const std::filesystem::path original = path_from_utf8(path);
    // Guard against a caller that passes a name which was not sanitized; the
    // limit also applies to the " (N)" variants.
    const std::string filename = path_to_utf8(original.filename());
    const std::filesystem::path p = candidate_path(original.parent_path(), filename, 0);
    if (!std::filesystem::exists(p, ec)) return path_to_utf8(p);
    for (int i = 1; i < 10000; ++i) {
        const std::filesystem::path candidate =
            candidate_path(original.parent_path(), filename, i);
        if (!std::filesystem::exists(candidate, ec)) return path_to_utf8(candidate);
    }
    return path_to_utf8(p);
}

// Picks a free name and moves the part file there in one atomic claim, so a
// concurrent writer can never be overwritten (audit finding 7).
bool move_file_unique(const std::string& from, const std::string& dir,
                      const std::string& filename, std::string* final_path,
                      std::string* error) {
    const std::filesystem::path out_dir = path_from_utf8(dir);
    for (int i = 0; i < 10000; ++i) {
        const std::string candidate = path_to_utf8(candidate_path(out_dir, filename, i));
        const ClaimResult claim = claim_path(from, candidate, error);
        if (claim == ClaimResult::Claimed) {
            if (final_path) *final_path = candidate;
            return true;
        }
        if (claim == ClaimResult::Failed) return false;
        // Exists: another file claimed the name, try the next one.
    }
    if (error) *error = "no free file name for '" + filename + "' in '" + dir + "'";
    return false;
}

DownloadEngine::DownloadEngine(Settings settings) : settings_(std::move(settings)) {}

namespace {

// The log keeps one file per download row, appended across runs so a resume
// shows the earlier attempts. Past the cap the old content is dropped instead
// of growing without bound (ponytail: no rotation beyond that).
constexpr uint64_t kMaxLogBytes = 1u << 20;

std::string log_timestamp() {
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    char buf[32] = {0};
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &tm);
    return buf;
}

void append_download_log(const std::string& path, const std::string& text) {
    if (path.empty()) return;
    const std::filesystem::path file = path_from_utf8(path);
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    if (!ec && std::filesystem::file_size(file, ec) > kMaxLogBytes && !ec) {
        std::filesystem::remove(file, ec);
    }
    const int fd = open_file(path, FileWrite | FileCreate | FileAppend);
    if (fd < 0) return;
    const std::string line = log_timestamp() + " " + text + "\n";
    write_file(fd, line.data(), line.size());
    close_file(fd);
}

// ServerCreationDate: give the finished file the date the server advertised.
// Only the modification time is set; Windows keeps its own creation time.
void apply_server_time(const std::string& path, int64_t server_time) {
    if (path.empty() || server_time <= 0) return;
    using std::chrono::system_clock;
    const auto delta = system_clock::from_time_t(static_cast<std::time_t>(server_time)) -
                       system_clock::now();
    std::error_code ec;
    std::filesystem::last_write_time(
        path_from_utf8(path), std::filesystem::file_time_type::clock::now() + delta, ec);
}

// Reports every run outcome through the same path, including the early
// failures that return without a transfer.
struct RunReporter {
    const DownloadResult& result;
    const Settings& settings;
    const ProbeResult& probe;
    std::string log_path;

    ~RunReporter() {
        switch (result.status) {
            case DownloadResult::Status::Completed:
                if (settings.server_creation_date) {
                    apply_server_time(result.path, probe.server_time);
                }
                append_download_log(log_path, "completed: " + result.path);
                break;
            case DownloadResult::Status::Paused:
                append_download_log(log_path, "paused");
                break;
            case DownloadResult::Status::Failed:
                append_download_log(log_path, "failed: " + result.error);
                break;
        }
    }
};

}  // namespace

std::string download_log_path(const std::string& log_dir, int64_t history_id,
                              const std::string& filename) {
    if (history_id <= 0 || log_dir.empty()) return {};
    const std::string name =
        limit_filename_length(sanitize_filename(filename), kMaxFileNameBytes);
    return path_to_utf8(path_from_utf8(log_dir) /
                        path_from_utf8(std::to_string(history_id) + "-" +
                                       (name.empty() ? "download" : name) + ".log"));
}

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
        append_download_log(
            download_log_path(default_log_dir(), request.history_id,
                              request.filename.empty() ? "probe" : request.filename),
            "probe failed: " + result.error);
        return result;
    }
    // A resolved link (Drive viewer links and virus-scan notices) names the URL
    // that actually serves the file; every transfer below must use it.
    if (!probe.download_url.empty()) options.url = probe.download_url;
    // A cross-origin redirect was followed by the probe without the caller's
    // headers or referer. Transfer the resolved URL directly and keep those
    // credentials off the new origin as well.
    const std::string resolved_url =
        probe.effective_url.empty() ? probe.download_url : probe.effective_url;
    if (!resolved_url.empty() && !same_url_origin(request.url, resolved_url)) {
        options.url = resolved_url;
        options.headers.clear();
        options.referer.clear();
        options.post_data.clear();
    }
    options.etag = probe.etag;
    options.last_modified = probe.last_modified;

    std::string filename = request.filename.empty() ? pick_filename(probe, request.url)
                                                    : sanitize_filename(request.filename);
    // The name becomes a path component on disk, so it has to fit the limit of
    // the filesystem no matter where it came from.
    filename = limit_filename_length(sanitize_filename(filename), kMaxFileNameBytes);
    if (filename.empty()) filename = "download.bin";

    RunReporter reporter{result, settings_, probe};
    reporter.log_path = download_log_path(default_log_dir(), request.history_id, filename);
    append_download_log(reporter.log_path,
                        "probe: size=" + std::to_string(probe.content_length) + ", ranges=" +
                            (probe.accept_ranges ? "yes" : "no") + ", etag=" + probe.etag +
                            ", last-modified=" + probe.last_modified);

    std::string out_dir =
        request.output_dir.empty() ? settings_.download_dir : request.output_dir;
    if (settings_.category_folders && request.apply_category_folder) {
        out_dir = path_to_utf8(path_from_utf8(out_dir) / path_from_utf8(category_folder_for(filename)));
    }

    std::string dir_error;
    if (!ensure_directory(out_dir, &dir_error) ||
        !ensure_private_directory(settings_.temp_dir, &dir_error)) {
        result.status = DownloadResult::Status::Failed;
        result.error = dir_error;
        return result;
    }

    const std::string workspace_key =
        probe.effective_url.empty() ? request.url : probe.effective_url;
    const DownloadWorkspace ws = make_workspace(settings_, workspace_key, filename);
    if (!ensure_private_directory(ws.dir, &dir_error)) {
        result.status = DownloadResult::Status::Failed;
        result.error = dir_error;
        return result;
    }
    result.workspace_dir = ws.dir;

    // ------------------------------------------------------------------
    // FTP and HLS take dedicated paths (04-protocols.md §2, HLS spec note).
    // ------------------------------------------------------------------
    if (is_ftp_url(workspace_key)) {
        result = run_ftp(options, probe, filename, out_dir, ws, request.resume, bandwidth_limit,
                         on_progress);
        return result;
    }
    if (is_hls_url(workspace_key, probe.mime_type)) {
        result = run_hls(options, probe,
                         limit_filename_length(hls_output_filename(filename), kMaxFileNameBytes),
                         out_dir, ws, request.resume, bandwidth_limit, on_progress);
        return result;
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
            std::string final_path;
            if (!move_file_unique(ws.part_path, out_dir, filename, &final_path, &move_error)) {
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
    append_download_log(reporter.log_path,
                        std::string(have_state ? "resume: " : "plan: ") +
                            std::to_string(state.segments.size()) + " segments");

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
        append_download_log(reporter.log_path,
                            "parallel ranges refused (403); retrying with one connection");
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
            std::string final_path;
            std::string move_error;
            if (!move_file_unique(ws.part_path, out_dir, filename, &final_path, &move_error)) {
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
