#pragma once

#include <QObject>
#include <QString>
#include <QVector>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "net/http.h"
#include "ui/media_variant.h"

namespace swiftdm::ui {

// Probes a media URL (HLS playlist, DASH manifest, or direct file) to discover
// available renditions.  Runs entirely on a worker thread; emits `finished`
// with the result list when done.  Used by the confirmation dialog to populate
// the quality selector before a download starts.
//
// Lifecycle: create → start() → finished() signal → deleteLater().
class MediaProbeTask : public QObject {
    Q_OBJECT
public:
    struct Options {
        std::string url;
        std::string referer;
        std::string user_agent;
        std::vector<swiftdm::HttpCookie> cookies;
        std::vector<swiftdm::HttpHeader> headers;
        std::string mime_type;  // from the extension's Content-Type hint
        std::string kind;       // "file" | "hls" | "dash" | "mse"
    };

    explicit MediaProbeTask(const Options& opts, QObject* parent = nullptr);
    ~MediaProbeTask() override;

    void start();
    void cancel();

signals:
    // Emitted on the main thread once probing is complete.
    // `variants` is empty on failure; `error` describes why.
    void finished(QVector<swiftdm::ui::MediaVariant> variants, QString error);

private:
    void run();

    Options opts_;
    std::atomic<bool> cancelled_{false};
    std::thread thread_;
};

}  // namespace swiftdm::ui
