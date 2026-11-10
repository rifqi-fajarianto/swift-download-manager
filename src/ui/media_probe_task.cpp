#include "ui/media_probe_task.h"

#include <QMetaObject>

#include "engine/dash.h"
#include "engine/hls.h"
#include "net/http.h"

namespace swiftdm::ui {

MediaProbeTask::MediaProbeTask(const Options& opts, QObject* parent)
    : QObject(parent), opts_(opts) {}

MediaProbeTask::~MediaProbeTask() {
    cancel();
    if (thread_.joinable()) thread_.join();
}

void MediaProbeTask::start() {
    thread_ = std::thread([this] { run(); });
}

void MediaProbeTask::cancel() { cancelled_.store(true, std::memory_order_relaxed); }

void MediaProbeTask::run() {
    QVector<MediaVariant> result;
    QString error;

    auto emit_result = [&] {
        QMetaObject::invokeMethod(this, [this, r = result, e = error] {
            emit finished(r, e);
        }, Qt::QueuedConnection);
    };

    // Determine type from kind hint, MIME hint, or URL.
    const bool hls = opts_.kind == "hls" || is_hls_url(opts_.url, opts_.mime_type);
    const bool dash = opts_.kind == "dash" || is_dash_url(opts_.url, opts_.mime_type);

    if (!hls && !dash) {
        // Direct file: single variant.
        HttpOptions ho;
        ho.url = opts_.url;
        ho.referer = opts_.referer;
        ho.user_agent = opts_.user_agent;
        ho.cookies = opts_.cookies;
        ho.headers = opts_.headers;
        ProbeResult pr = probe_url(ho);
        if (cancelled_.load(std::memory_order_relaxed)) return emit_result();
        if (!pr.ok) {
            error = QString::fromStdString(pr.error);
            return emit_result();
        }
        MediaVariant v;
        v.kind = "file";
        v.url = opts_.url;
        v.label = "default";
        v.size_estimate = pr.content_length;
        result.push_back(std::move(v));
        return emit_result();
    }

    // Fetch manifest text.
    HttpOptions ho;
    ho.url = opts_.url;
    ho.referer = opts_.referer;
    ho.user_agent = opts_.user_agent;
    ho.cookies = opts_.cookies;
    ho.headers = opts_.headers;

    std::string body, effective_url, fetch_error;
    if (!fetch_text(ho, &body, &effective_url, &fetch_error)) {
        error = QString::fromStdString(fetch_error);
        return emit_result();
    }
    if (cancelled_.load(std::memory_order_relaxed)) return emit_result();

    if (hls) {
        HlsPlaylist playlist;
        std::string parse_error;
        if (!parse_hls_playlist(body, &playlist, &parse_error)) {
            error = QString::fromStdString(parse_error);
            return emit_result();
        }
        if (playlist.encrypted) {
            error = QStringLiteral("encrypted HLS stream");
            return emit_result();
        }
        if (playlist.master) {
            for (auto& [bw, uri] : playlist.variants) {
                MediaVariant v;
                v.kind = "hls";
                v.bandwidth = bw;
                v.url = resolve_playlist_uri(effective_url, uri);
                v.manifest_url = effective_url;
                v.label = media_variant_label(v);
                result.push_back(std::move(v));
            }
        } else {
            MediaVariant v;
            v.kind = "hls";
            v.url = effective_url;
            v.manifest_url = effective_url;
            v.label = "default";
            result.push_back(std::move(v));
        }
    } else {
        // DASH
        DashManifest manifest;
        std::string parse_error;
        if (!parse_dash_manifest(body, effective_url, &manifest, &parse_error)) {
            error = QString::fromStdString(parse_error);
            return emit_result();
        }
        for (auto& v : manifest.video_representations) result.push_back(std::move(v));
        for (auto& v : manifest.audio_representations) result.push_back(std::move(v));
    }

    emit_result();
}

}  // namespace swiftdm::ui
