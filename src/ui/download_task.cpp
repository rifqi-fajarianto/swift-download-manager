#include "ui/download_task.h"

#include "engine/download_engine.h"
#include "net/http.h"

namespace swiftdm::ui {

namespace {

swiftdm::HttpOptions http_options_for(const swiftdm::Settings& settings,
                                      const DownloadTask::Options& options) {
    swiftdm::HttpOptions http;
    http.url = options.url.toStdString();
    http.user_agent = options.user_agent.isEmpty() ? settings.default_agent
                                                   : options.user_agent.toStdString();
    http.referer = options.referer.toStdString();
    http.cookies = options.cookies;
    http.headers = options.headers;
    http.post_data.assign(options.post_data.constData(),
                          static_cast<size_t>(options.post_data.size()));
    if (settings.proxy.type != "none" && !settings.proxy.address.empty()) {
        http.proxy_type = settings.proxy.type;
        http.proxy_address = settings.proxy.address;
        http.proxy_port = settings.proxy.port;
        http.proxy_user = settings.proxy.user;
        http.proxy_password = settings.proxy.password;
    }
    return http;
}

}  // namespace

DownloadTask::DownloadTask(swiftdm::Settings settings, Options options, QObject* parent)
    : QObject(parent), settings_(std::move(settings)), options_(std::move(options)) {}

DownloadTask::~DownloadTask() {
    cancel_requested_ = true;
    pause_requested_ = true;
    if (thread_.joinable()) thread_.join();
}

void DownloadTask::set_options(const Options& options) {
    if (active()) return;
    options_ = options;
}

QString DownloadTask::resolved_filename() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return resolved_filename_;
}

quint64 DownloadTask::resolved_size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return resolved_size_;
}

bool DownloadTask::resumable() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return resumable_;
}

QString DownloadTask::workspace_dir() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return workspace_dir_;
}

DownloadTask::Snapshot DownloadTask::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_;
}

void DownloadTask::start() {
    if (active_.exchange(true)) return;
    if (thread_.joinable()) thread_.join();
    pause_requested_ = false;
    cancel_requested_ = false;
    thread_ = std::thread([this] { run(); });
}

void DownloadTask::start_probe() {
    if (active_.exchange(true)) return;
    if (thread_.joinable()) thread_.join();
    pause_requested_ = false;
    cancel_requested_ = false;
    thread_ = std::thread([this] { run_probe(); });
}

void DownloadTask::run_probe() {
    const swiftdm::ProbeResult probe =
        swiftdm::probe_url(http_options_for(settings_, options_));
    if (cancel_requested_.load()) {
        active_ = false;
        emit probe_completed(false, QString());
        return;
    }
    if (!probe.ok) {
        active_ = false;
        const QString message =
            QString::fromStdString(probe.error.empty() ? "probe failed" : probe.error);
        emit probe_completed(false, message);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        resolved_filename_ = options_.filename.isEmpty()
                                 ? QString::fromStdString(probe.filename_hint)
                                 : options_.filename;
        resolved_size_ = probe.content_length;
        resumable_ = probe.accept_ranges;
    }
    emit probed(resolved_filename(), resolved_size(), resumable());
    active_ = false;
    emit probe_completed(true, QString());
}

void DownloadTask::pause() {
    if (active()) pause_requested_ = true;
}

void DownloadTask::cancel() {
    if (active()) cancel_requested_ = true;
}

void DownloadTask::run() {
    // Probe first so the UI can show the resolved name/size immediately.
    const swiftdm::ProbeResult probe =
        swiftdm::probe_url(http_options_for(settings_, options_));
    if (!probe.ok) {
        active_ = false;
        const QString message =
            QString::fromStdString(probe.error.empty() ? "probe failed" : probe.error);
        emit finished(static_cast<int>(Status::Failed), QString(), message);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        resolved_filename_ = options_.filename.isEmpty()
                                 ? QString::fromStdString(probe.filename_hint)
                                 : options_.filename;
        resolved_size_ = probe.content_length;
        resumable_ = probe.accept_ranges;
    }
    emit probed(resolved_filename(), resolved_size(), resumable());

    swiftdm::DownloadRequest request;
    request.url = options_.url.toStdString();
    request.output_dir = options_.output_dir.toStdString();
    request.filename = options_.filename.toStdString();
    request.referer = options_.referer.toStdString();
    request.cookies = options_.cookies;
    request.user_agent = options_.user_agent.toStdString();
    request.connections = options_.connections;
    request.bandwidth_limit_kbps = options_.limit_kbps;
    request.headers = options_.headers;
    request.post_data.assign(options_.post_data.constData(),
                             static_cast<size_t>(options_.post_data.size()));

    swiftdm::DownloadEngine engine(settings_);
    const swiftdm::DownloadResult result =
        engine.run(request, [this](const swiftdm::ProgressInfo& info) {
            if (cancel_requested_.load() || pause_requested_.load()) return false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                snapshot_.done = info.done;
                snapshot_.total = info.total;
                snapshot_.speed_bps = info.speed_bps;
                snapshot_.percent = info.percent;
                snapshot_.segments.clear();
                snapshot_.segments.reserve(static_cast<qsizetype>(info.segments.size()));
                for (const swiftdm::Segment& segment : info.segments) {
                    snapshot_.segments.append({segment.done, segment.length()});
                }
            }
            emit progressed();
            return true;
        });

    Status status = Status::Completed;
    if (cancel_requested_.load()) {
        status = Status::Canceled;
    } else if (result.status == swiftdm::DownloadResult::Status::Paused) {
        status = Status::Paused;
    } else if (result.status == swiftdm::DownloadResult::Status::Failed) {
        status = Status::Failed;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.speed_bps = 0.0;
        if (!result.workspace_dir.empty()) {
            workspace_dir_ = QString::fromStdString(result.workspace_dir);
        }
    }
    active_ = false;
    emit finished(static_cast<int>(status), QString::fromStdString(result.path),
                  QString::fromStdString(result.error));
}

}  // namespace swiftdm::ui
