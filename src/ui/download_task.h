#pragma once

#include <QByteArray>
#include <QObject>
#include <QPair>
#include <QString>
#include <QVector>

#include <atomic>
#include <mutex>
#include <thread>

#include "net/http.h"
#include "store/settings.h"

namespace swiftdm::ui {

// Runs one download (probe + segmented transfer) on a worker thread.
class DownloadTask : public QObject {
    Q_OBJECT
  public:
    struct Options {
        QString url;
        QString output_dir;  // empty = settings default
        QString filename;    // empty = from server/URL
        QString category;    // metadata for the history/category tree
        // True appends the extension-derived category folder (legacy). The GUI
        // uses an exact output_dir chosen in the confirmation dialog.
        bool apply_category_folder = true;
        QString referer;
        std::vector<swiftdm::HttpCookie> cookies;
        QString user_agent;
        std::vector<swiftdm::HttpHeader> headers;  // extra request headers
        QByteArray post_data;  // non-empty = POST handoff body
        int connections = 0;   // 0 = settings
        int limit_kbps = 0;    // 0 = settings
    };

    struct Snapshot {
        quint64 done = 0;
        quint64 total = 0;
        double speed_bps = 0.0;
        int percent = 0;
        QVector<QPair<quint64, quint64>> segments;  // (done, length) per segment
    };

    // Values carried by the finished() signal (kept as int for queued
    // connections).
    enum class Status { Completed = 0, Paused = 1, Failed = 2, Canceled = 3 };

    DownloadTask(swiftdm::Settings settings, Options options, QObject* parent = nullptr);
    ~DownloadTask() override;

    const Options& options() const { return options_; }
    void set_options(const Options& options);

    bool active() const { return active_.load(); }
    QString resolved_filename() const;
    quint64 resolved_size() const;
    bool resumable() const;
    QString workspace_dir() const;
    Snapshot snapshot() const;

    void start();        // initial run or resume of a paused/failed task
    void start_probe();  // probe only; used by the manual-add confirmation
    void pause();   // request pause; partial state is kept for resume
    void cancel();  // abort; state file is kept until the row is removed
    // Requests a pause (when running) and waits for the worker thread. Used on
    // application shutdown so segments.json is written before the process ends.
    void pause_and_wait();
    // Seeds the snapshot for a task restored from history (not running).
    void restore_progress(quint64 done, quint64 total, bool resumable);
    // Seeds the workspace directory for a task restored from history, so the
    // Properties dialog and Delete cleanup know the temp folder.
    void set_workspace_dir(const QString& dir);

  signals:
    void probed(const QString& filename, quint64 size, bool resumable);
    void probe_completed(bool ok, const QString& error);
    void progressed();
    void finished(int status, const QString& path, const QString& error);

  private:
    void run();
    void run_probe();

    swiftdm::Settings settings_;
    Options options_;
    std::thread thread_;
    std::atomic<bool> active_{false};
    std::atomic<bool> pause_requested_{false};
    std::atomic<bool> cancel_requested_{false};

    mutable std::mutex mutex_;
    Snapshot snapshot_;
    QString resolved_filename_;
    quint64 resolved_size_ = 0;
    bool resumable_ = false;
    QString workspace_dir_;
};

}  // namespace swiftdm::ui
