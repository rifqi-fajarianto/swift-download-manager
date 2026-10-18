#include "engine/download_engine.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>

#include "core/log.h"
#include "core/util.h"
#include "engine/categories.h"
#include "engine/segment_plan.h"

namespace swiftdm {
namespace {

int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

uint64_t fnv1a(const std::string& text) {
    uint64_t hash = 1469598103934665603ULL;
    for (unsigned char c : text) {
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::string hex64(uint64_t value) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(value));
    return buf;
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
    std::filesystem::rename(from, to, ec);
    if (!ec) return true;
    // Cross-device move fallback.
    std::error_code copy_ec;
    std::filesystem::copy_file(from, to, std::filesystem::copy_options::overwrite_existing,
                               copy_ec);
    if (copy_ec) {
        if (error) *error = "cannot move to '" + to + "': " + copy_ec.message();
        return false;
    }
    std::filesystem::remove(from, ec);
    return true;
}

}  // namespace

DownloadWorkspace make_workspace(const Settings& settings, const std::string& url,
                                 const std::string& filename) {
    DownloadWorkspace ws;
    const std::filesystem::path dir =
        std::filesystem::path(settings.temp_dir) /
        ("download-" + hex64(fnv1a(url + "\n" + filename)));
    ws.dir = dir.string();
    ws.part_path = (dir / (filename + ".part")).string();
    ws.state_path = (dir / "segments.json").string();
    return ws;
}

std::string unique_path(const std::string& path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return path;
    const std::filesystem::path p(path);
    const std::string stem = p.stem().string();
    const std::string ext = p.extension().string();
    for (int i = 1; i < 10000; ++i) {
        const std::filesystem::path candidate =
            p.parent_path() / (stem + " (" + std::to_string(i) + ")" + ext);
        if (!std::filesystem::exists(candidate, ec)) return candidate.string();
    }
    return path;
}

DownloadEngine::DownloadEngine(Settings settings) : settings_(std::move(settings)) {}

DownloadResult DownloadEngine::run(
    const DownloadRequest& request,
    const std::function<bool(const ProgressInfo&)>& on_progress) {
    DownloadResult result;

    const int connections =
        request.connections > 0 ? request.connections : settings_.max_connections;
    const int limit_kbps = request.bandwidth_limit_kbps > 0 ? request.bandwidth_limit_kbps
                                                            : settings_.bandwidth_limit_kbps;

    HttpOptions options;
    options.url = request.url;
    options.headers = request.headers;
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

    const ProbeResult probe = probe_url(options);
    if (!probe.ok) {
        result.status = DownloadResult::Status::Failed;
        result.error = probe.error.empty() ? "probe failed" : probe.error;
        return result;
    }

    std::string filename = request.filename.empty() ? pick_filename(probe, request.url)
                                                    : sanitize_filename(request.filename);
    if (filename.empty()) filename = "download.bin";

    std::string out_dir =
        request.output_dir.empty() ? settings_.download_dir : request.output_dir;
    if (settings_.category_folders) {
        out_dir = (std::filesystem::path(out_dir) / category_folder_for(filename)).string();
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

    const bool can_resume = probe.accept_ranges && request.post_data.empty();
    const bool known_size = probe.content_length > 0;

    // ------------------------------------------------------------------
    // Unknown size: sequential streaming, no resume (spec 03-engine.md).
    // ------------------------------------------------------------------
    if (!known_size) {
        const int fd = ::open(ws.part_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
                              0644);
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
            stream_download(options, fd, progress, &written, &transfer_error);
        ::fsync(fd);
        ::close(fd);

        if (outcome == TransferOutcome::Completed) {
            std::string move_error;
            const std::string final_path =
                unique_path((std::filesystem::path(out_dir) / filename).string());
            if (!move_file(ws.part_path, final_path, &move_error)) {
                result.status = DownloadResult::Status::Failed;
                result.error = move_error;
                return result;
            }
            std::error_code ec;
            std::filesystem::remove_all(ws.dir, ec);
            result.status = DownloadResult::Status::Completed;
            result.path = final_path;
            result.size = written;
            return result;
        }

        std::error_code ec;
        std::filesystem::remove(ws.part_path, ec);
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

    const int fd = ::open(ws.part_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) {
        result.status = DownloadResult::Status::Failed;
        result.error = "cannot create '" + ws.part_path + "': " + errno_string(errno);
        return result;
    }
    if (::ftruncate(fd, static_cast<off_t>(state.file_size)) != 0) {
        ::close(fd);
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
        const int64_t t = now_ms();
        if (request.resume && t - last_save >= 2000) {
            last_save = t;
            save_transfer_state(ws.state_path, state, nullptr);
        }
        return on_progress ? on_progress(info) : true;
    };

    SegmentDownloader downloader(options, state, fd);
    downloader.set_one_by_one(settings_.connections_at_once != 0);
    downloader.set_bandwidth_limit_bytes(static_cast<int64_t>(limit_kbps) * 1024);
    std::string transfer_error;
    const TransferOutcome outcome = downloader.run(progress, &transfer_error);
    ::fsync(fd);
    ::close(fd);

    switch (outcome) {
        case TransferOutcome::Completed: {
            if (state.done_bytes() != state.file_size) {
                save_transfer_state(ws.state_path, state, nullptr);
                result.status = DownloadResult::Status::Failed;
                result.error = "incomplete download";
                return result;
            }
            const std::string final_path =
                unique_path((std::filesystem::path(out_dir) / filename).string());
            std::string move_error;
            if (!move_file(ws.part_path, final_path, &move_error)) {
                save_transfer_state(ws.state_path, state, nullptr);
                result.status = DownloadResult::Status::Failed;
                result.error = move_error;
                return result;
            }
            remove_transfer_state(ws.state_path);
            std::error_code ec;
            std::filesystem::remove_all(ws.dir, ec);
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
