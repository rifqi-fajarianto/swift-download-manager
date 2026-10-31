#include "ui/main_window.h"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QDateTime>
#include <QDebug>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QHeaderView>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QJsonArray>
#include <QLabel>
#include <QJsonObject>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QStyleHints>
#include <QSystemTrayIcon>
#include <QTableView>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>
#include <memory>
#include <optional>

#include "core/util.h"
#include "engine/categories.h"
#include "engine/segment_plan.h"
#include "swiftdm/version.h"
#include "ui/add_download_dialog.h"
#include "ui/browser_integration.h"
#include "ui/confirm_download_dialog.h"
#include "ui/download_model.h"
#include "ui/download_window.h"
#include "ui/icons.h"
#include "ui/properties_dialog.h"
#include "ui/queue_dialog.h"
#include "ui/refresh_logic.h"
#include "ui/refresh_waiting_dialog.h"
#include "ui/settings_dialog.h"
#include "ui/theme.h"

#include "ui/main_window_internal.h"

namespace swiftdm::ui {

namespace {

// Rewrites the saved transfer state after a URL change: keeps segment progress
// and file size, drops the old validators (the new address is a new server
// resource identity) and records the new URL.
void rewrite_resume_state(const QString& workspace, const QString& new_url) {
    const QString state_path = workspace + QStringLiteral("/segments.json");
    if (!QFile::exists(state_path)) return;
    swiftdm::TransferState state;
    if (!swiftdm::load_transfer_state(state_path.toStdString(), state, nullptr)) {
        qWarning() << "refresh address: cannot read" << state_path;
        return;
    }
    state.url = new_url.toStdString();
    state.etag.clear();
    state.last_modified.clear();
    std::string error;
    if (!swiftdm::save_transfer_state(state_path.toStdString(), state, &error)) {
        qWarning() << "refresh address: cannot rewrite" << state_path
                   << QString::fromStdString(error);
    }
}

}  // namespace

void MainWindow::on_new_url() {
    // A waiting refresh owns the next capture; adding a manual download now
    // would steal it. Warn instead of opening the New URL dialog.
    if (refresh_blocks_new_download(pending_refresh_id_ > 0)) {
        warn_refresh_blocks_new_download();
        return;
    }
    AddDownloadDialog dialog(this);
    if (dialog.exec() != QDialog::Accepted) return;
    const QString url = dialog.url();
    if (url.isEmpty()) return;
    add_download(url, selected_queue_id_);
}

void MainWindow::add_download(const QString& url, qint64 queue_id) {
    const QUrl parsed = QUrl::fromUserInput(url.trimmed());
    const QString scheme = parsed.scheme().toLower();
    const bool supported_scheme = scheme == QStringLiteral("http") ||
                                  scheme == QStringLiteral("https") ||
                                  scheme == QStringLiteral("ftp") ||
                                  scheme == QStringLiteral("ftps");
    if (!parsed.isValid() || !supported_scheme || parsed.host().isEmpty()) {
        show_error_dialog(url, QString::fromLatin1(swiftdm::kInvalidUrlError));
        return;
    }
    const QString request_url = parsed.toString(QUrl::FullyEncoded);

    DownloadTask::Options options;
    options.url = request_url;
    options.output_dir = QString::fromStdString(settings_.download_dir);
    // New downloads start from the global limiter state; the Options tab can
    // override both the toggle and the value per download. The row keeps this
    // value even when it is added to a queue: the queue ignores it while the
    // item belongs there, but a later move back to Downloads can use it again.
    options.limit_enabled = settings_.bandwidth_limit_enabled;
    options.limit_kbps = settings_.bandwidth_limit_kbps;

    DownloadItem item;
    item.url = request_url;
    item.file_name = QString::fromStdString(
        swiftdm::file_name_from_url(request_url.toStdString()));
    item.category =
        QString::fromStdString(swiftdm::category_folder_for(item.file_name.toStdString()));
    item.output_dir = options.output_dir;
    item.queue_id = queue_id;
    item.bandwidth_limit_enabled = options.limit_enabled;
    item.bandwidth_limit_kbps = options.limit_kbps;
    probe_and_confirm(std::move(options), std::move(item));
}

void MainWindow::add_browser_download(const QJsonObject& data) {
    const QUrl url(data.value(QStringLiteral("url")).toString());
    if (!url.isValid() || (url.scheme() != QStringLiteral("http") &&
                           url.scheme() != QStringLiteral("https") &&
                           url.scheme() != QStringLiteral("ftp")) ||
        url.host().isEmpty()) {
        return;
    }

    // Refresh interception: while a download waits for a new address, no other
    // download may be created. Every capture goes to the confirmation prompt; a
    // rejected one leaves the wait in place instead of becoming a new download.
    if (should_ignore_refresh_echo(
            url.toString(QUrl::FullyEncoded).toStdString(),
            refresh_applied_url_.toStdString(),
            QDateTime::currentMSecsSinceEpoch() - refresh_applied_at_ms_)) {
        // The extension retried the handoff that was just applied. Creating a
        // download here is exactly the duplicate-prompt bug this guards against.
        qInfo() << "refresh address: ignoring repeated capture for the applied URL";
        return;
    }

    if (pending_refresh_id_ > 0) {
        int waiting_row = -1;
        for (int r = 0; r < model_->rowCount(); ++r) {
            const DownloadItem* it = model_->item_at(r);
            if (it && it->history_id == pending_refresh_id_) {
                waiting_row = r;
                break;
            }
        }
        if (waiting_row < 0) {
            // The waiting row disappeared (deleted behind our back).
            pending_refresh_id_ = 0;
            hide_refresh_waiting_dialog();
        } else {
            DownloadItem* waiting_item = model_->item_at(waiting_row);
            const std::string raw_hint = swiftdm::sanitize_filename(
                data.value(QStringLiteral("filename")).toString().toStdString());
            const QString incoming_filename = raw_hint.empty()
                ? QString::fromStdString(swiftdm::file_name_from_url(url.toString().toStdString()))
                : QString::fromStdString(raw_hint);
            const bool name_match = incoming_filename.compare(
                waiting_item->file_name, Qt::CaseInsensitive) == 0;
            const RefreshCaptureDecision decision = decide_refresh_capture(true, name_match);

            // Hide the waiting window while the prompt is up: only one of the
            // two may be visible. It comes back when the user cancels.
            hide_refresh_waiting_dialog();

            QMessageBox box(this);
            box.setWindowTitle(tr("Confirm New Download URL"));
            box.setIconPixmap(decision == RefreshCaptureDecision::PromptNameMismatch
                                  ? dialog_warning_pixmap()
                                  : dialog_question_pixmap());
            box.setTextFormat(Qt::RichText);
            QString html =
                tr("<p>Is this the new URL for your expired download?</p>"
                   "<table cellspacing='0' cellpadding='2'>"
                   "<tr><td style='color:#9aa0a6;'>File Name:</td>"
                   "<td>%1</td></tr>"
                   "<tr><td style='color:#9aa0a6;'>New URL:</td>"
                   "<td>%2</td></tr>"
                   "</table>")
                    .arg(waiting_item->file_name.toHtmlEscaped(),
                         url.toString(QUrl::FullyEncoded).toHtmlEscaped());
            if (decision == RefreshCaptureDecision::PromptNameMismatch) {
                // The captured file is not the one being refreshed: make this
                // impossible to miss with the theme's danger red.
                html += tr("<p style='color:%1;'><b>The file name does not match the "
                           "waiting download.</b><br/>"
                           "Use this URL only if it really is the same file.</p>")
                            .arg(colors::danger_tint().name());
            }
            box.setText(html);

            QPushButton* use_button = box.addButton(tr("Use New URL"),
                                                    QMessageBox::AcceptRole);
            QPushButton* cancel_button = box.addButton(tr("Cancel"),
                                                       QMessageBox::RejectRole);
            box.setDefaultButton(decision == RefreshCaptureDecision::Prompt
                                     ? use_button
                                     : cancel_button);
            box.setEscapeButton(cancel_button);
            box.exec();
            if (box.clickedButton() == use_button) {
                apply_refresh_address(waiting_row, data);
            }
            // Rejected (or applied): never fall through to a new download. On a
            // rejection the waiting window is shown again so the user can retry.
            if (pending_refresh_id_ > 0) show_refresh_waiting_dialog(
                pending_refresh_id_, waiting_item->file_name, waiting_item->size);
            return;
        }
    }

    DownloadTask::Options options;
    options.url = url.toString(QUrl::FullyEncoded);
    options.output_dir = QString::fromStdString(settings_.download_dir);
    options.limit_enabled = settings_.bandwidth_limit_enabled;
    options.limit_kbps = settings_.bandwidth_limit_kbps;
    const std::string requested_filename = swiftdm::sanitize_filename(
        data.value(QStringLiteral("filename")).toString().toStdString());
    options.filename = QString::fromStdString(requested_filename);
    options.referer = data.value(QStringLiteral("referer")).toString();
    if (!options.referer.isEmpty()) {
        const QUrl referer(options.referer);
        const bool same_origin = referer.scheme() == url.scheme() &&
                                 referer.host().compare(url.host(), Qt::CaseInsensitive) == 0 &&
                                 referer.port(0) == url.port(0);
        if (!same_origin) options.referer.clear();
    }
    options.user_agent = data.value(QStringLiteral("userAgent")).toString();
    if (settings_.use_ua_for_browser) options.user_agent.clear();
    options.headers = headers_from_json(data.value(QStringLiteral("headers")));
    options.post_data = decode_post_data(data);
    options.cookies = cookies_from_json(data.value(QStringLiteral("cookies")));

    qInfo() << "browser integration: confirming download" << options.url;
    DownloadItem item;
    item.url = options.url;
    item.file_name = options.filename.isEmpty()
                         ? QString::fromStdString(
                               swiftdm::file_name_from_url(options.url.toStdString()))
                         : options.filename;
    item.category = QString::fromStdString(
        swiftdm::category_folder_for(item.file_name.toStdString()));
    item.output_dir = options.output_dir;
    // Keep the global per-item limit as the row's stored value. A queue item
    // ignores it while it belongs to a queue; moving it back can use it again.
    item.bandwidth_limit_enabled = options.limit_enabled;
    item.bandwidth_limit_kbps = options.limit_kbps;
    item.page_url = data.value(QStringLiteral("pageUrl")).toString();
    item.page_title = data.value(QStringLiteral("pageTitle")).toString();
    // Adaptive video+audio handoffs share a token (validated by the protocol
    // layer) so both rows can be recognized as one pair.
    item.group = data.value(QStringLiteral("group")).toString();
    probe_and_confirm(std::move(options), std::move(item));
}

void MainWindow::probe_and_confirm(DownloadTask::Options options, DownloadItem item) {
    item.url = options.url;
    DownloadTask::Options probe_options = options;
    // The confirmation shows the name the server provides. Extension-provided
    // names stay as a fallback for servers without Content-Disposition.
    probe_options.filename.clear();

    struct ProbeFlow {
        DownloadTask::Options options;
        DownloadItem item;
        QPointer<ConfirmDownloadDialog> dialog;
        QPointer<DownloadTask> probe_task;
        swiftdm::ProbeResult probe_result;
        QString resolved_name;
        quint64 resolved_size = 0;
        bool resumable = false;
        bool ready = false;
        bool canceled = false;
        qint64 started_ms = 0;
        std::function<void()> start_download;
    };

    auto flow = std::make_shared<ProbeFlow>();
    flow->options = std::move(options);
    flow->item = std::move(item);
    flow->started_ms = now_ms();

    const bool show_confirmation = settings_.show_download_confirmation;
    if (show_confirmation) {
        QString fallback_name = flow->options.filename.isEmpty()
                                    ? flow->item.file_name
                                    : flow->options.filename;
        if (fallback_name.isEmpty()) fallback_name = QStringLiteral("download.bin");
        auto* dialog = new ConfirmDownloadDialog(
            fallback_name, 0, flow->options.url,
            QString::fromStdString(settings_.download_dir), settings_.category_dirs,
            settings_.category_folders);
        dialog->set_queue_choices(queue_choices(), flow->item.queue_id);
        // Requested by the user in Settings: keep the confirmation above every
        // other window. The flag must be set before the first show.
        dialog->setWindowFlag(Qt::WindowStaysOnTopHint, settings_.confirm_dialog_on_top);
        dialog->set_probe_pending();
        flow->dialog = dialog;
        connect(dialog, &QDialog::finished, this, [flow](int result) {
            if (result != QDialog::Accepted) {
                flow->canceled = true;
                if (flow->probe_task) flow->probe_task->cancel();
                if (flow->dialog) flow->dialog->deleteLater();
            } else if (flow->ready && flow->start_download) {
                flow->start_download();
            }
        });
        dialog->setModal(true);
        // Top-level dialog intentionally has no parent: browser handoffs must
        // not restore a main window that is hidden to the tray.
    }

    auto* probe = new DownloadTask(settings_, probe_options, this);
    flow->probe_task = probe;
    qInfo() << "download latency: probe started";
    connect(probe, &DownloadTask::probe_completed, this,
            [this, probe, flow, show_confirmation](bool ok, const QString& error) {
                if (flow->canceled) {
                    probe->deleteLater();
                    return;
                }
                const QString probed_name = probe->resolved_filename();
                const quint64 probed_size = probe->resolved_size();
                const bool resumable = probe->resumable();
                const swiftdm::ProbeResult probe_result = probe->probe_result();
                const qint64 probe_ms = now_ms() - flow->started_ms;
                qInfo() << "download latency: probe completed" << probe_ms << "ms";

                if (!ok) {
                    if (flow->dialog) flow->dialog->reject();
                    if (!error.isEmpty())
                        show_error_dialog(flow->options.url, error, flow->item.page_url);
                    probe->deleteLater();
                    return;
                }

                QString name = QString::fromStdString(
                    swiftdm::sanitize_filename(probed_name.toStdString()));
                if (name.isEmpty()) {
                    name = QString::fromStdString(
                        swiftdm::sanitize_filename(flow->item.file_name.toStdString()));
                }
                if (name.isEmpty()) name = QStringLiteral("download.bin");

                if (show_confirmation) {
                    if (!flow->dialog) {
                        probe->deleteLater();
                        return;  // user canceled while the probe was in flight
                    }
                    flow->dialog->set_probe_result(name, probed_size);
                    qInfo() << "download latency: confirmation metadata ready"
                            << now_ms() - flow->started_ms << "ms";
                }

                flow->resolved_name = name;
                flow->resolved_size = probed_size;
                flow->resumable = resumable;
                flow->probe_result = probe_result;
                flow->ready = true;
                probe->deleteLater();
                if (!show_confirmation && flow->start_download) flow->start_download();
            });

    const std::weak_ptr<ProbeFlow> weak_flow = flow;
    flow->start_download = [this, weak_flow] {
        const std::shared_ptr<ProbeFlow> flow = weak_flow.lock();
        if (!flow) return;
        if (flow->canceled || !flow->ready) return;
        qInfo() << "download latency: download accepted"
                << now_ms() - flow->started_ms << "ms";
        DownloadItem draft = flow->item;
        draft.url = flow->options.url;
        draft.file_name = flow->resolved_name;
        draft.category = QString::fromStdString(
            swiftdm::category_folder_for(flow->resolved_name.toStdString()));
        draft.size = flow->resolved_size;
        draft.resumable = flow->resumable;
        draft.output_dir = QString::fromStdString(settings_.download_dir);
        draft.last_try = now_stamp();
        draft.last_try_epoch = swiftdm::unix_now();
        draft.state = DownloadItem::State::Queued;

        DownloadTask::Options start_options = flow->options;
        start_options.filename = flow->resolved_name;
        start_options.category = draft.category;
        qint64 target_queue = flow->item.queue_id;
        if (flow->dialog) {
            draft.category = flow->dialog->category();
            draft.output_dir = flow->dialog->location();
            start_options.category = draft.category;
            start_options.apply_category_folder = false;
            start_options.output_dir = draft.output_dir;
            // Download Now always means the main list; Add to queue keeps the
            // queue chosen in the confirmation dialog (or the queue the
            // download came from, which the dialog resolves without a picker).
            target_queue = flow->dialog->selected_queue_id();
            flow->dialog->deleteLater();
            flow->dialog = nullptr;
        } else {
            draft.output_dir = default_download_location(
                QString::fromStdString(settings_.download_dir), settings_.category_dirs,
                draft.category, settings_.category_folders);
            start_options.output_dir = draft.output_dir;
            start_options.apply_category_folder = false;
        }
        draft.queue_id = target_queue;
        if (target_queue > 0) {
            const swiftdm::HistoryStore::QueueRecord* queue = queue_for(target_queue);
            // A queue overrides the per-item connection count; 0 there means
            // the global Settings value.
            start_options.connections = queue ? queue->connections : 0;
            // The queue budget replaces the per-item limiter for the transfer;
            // the row keeps its stored per-item value for a later move back.
            start_options.limit_enabled = false;
            start_options.limit_kbps = 0;
        }
        start_confirmed_download(std::move(draft), std::move(start_options),
                                 flow->probe_result);
    };

    if (flow->dialog) {
        qInfo() << "download latency: confirmation shown" << now_ms() - flow->started_ms
                << "ms";
        flow->dialog->open();
    }
    probe->start_probe();
}

void MainWindow::start_confirmed_download(DownloadItem item, DownloadTask::Options options,
                                          const swiftdm::ProbeResult& probe) {
    // The queue can disappear while a background probe runs (confirmation
    // disabled); fall back to the main list instead of hiding the row in a
    // workspace that no longer exists.
    if (item.queue_id > 0 && !queue_for(item.queue_id)) item.queue_id = 0;
    const qint64 queue_id = item.queue_id;
    const bool queue_item = queue_id > 0;
    if (queue_item) {
        item.state = DownloadItem::State::Queued;
        const swiftdm::HistoryStore::QueueRecord* queue = queue_for(queue_id);
        // The queue's connection rule always wins; 0 means global Settings.
        options.connections = queue ? queue->connections : 0;
    } else {
        item.bandwidth_limit_enabled = options.limit_enabled;
        item.bandwidth_limit_kbps = options.limit_kbps;
        item.connections = options.connections;
    }
    const int row = model_->add_item(item);
    DownloadItem* added = model_->item_at(row);
    if (!added) return;
    if (history_.is_open()) {
        added->history_id = history_.insert(history_record_for(*added, &options));
    }
    if (queue_item) {
        // A new item means the queue is no longer complete: stop any pending
        // shutdown countdown, then let the scheduler decide whether a slot is
        // free. No progress window opens for a queued item.
        //
        // Keep an inactive task as the option carrier so the scheduler (and a
        // later Start) still has the cookies, headers and POST body that the
        // browser/confirmation supplied.
        auto* queued_task = new DownloadTask(settings_, options, model_);
        added->task = queued_task;
        wire_task(queued_task);
        cancel_queue_shutdown(queue_id);
        complete_notified_.remove(queue_id);
        if (running_queues_.contains(queue_id)) schedule_queue(queue_id);
        rebuild_queue_tree();
        update_status_bar();
        return;
    }
    const int id = added->id;
    start_task_at_row(row, &options, &probe);
    open_download_window(id);
}

void MainWindow::start_task_at_row(int row, const DownloadTask::Options* options_override,
                                   const swiftdm::ProbeResult* probe) {
    DownloadItem* item = model_->item_at(row);
    if (!item) return;

    // Replace the previous (inactive) task so that current settings apply, but
    // keep its options: they carry cookies, headers and POST data that cannot
    // be rebuilt from the row alone.
    std::optional<DownloadTask::Options> previous_options;
    if (item->task && !item->task->active()) {
        previous_options = item->task->options();
        item->task->deleteLater();
        item->task = nullptr;
    }

    if (item->task && item->task->active()) return;

    const qint64 queue_id = item->queue_id;
    const bool queue_item = queue_id > 0;
    const swiftdm::HistoryStore::QueueRecord* queue = queue_item ? queue_for(queue_id) : nullptr;

    DownloadTask::Options options;
    if (options_override) {
        options = *options_override;
    } else if (previous_options) {
        options = *previous_options;
    } else {
        options.url = item->url;
        options.output_dir = item->output_dir;
        options.category = item->category;
        options.connections = 0;   // 0 = settings
        options.limit_enabled = item->bandwidth_limit_enabled;
        options.limit_kbps = item->bandwidth_limit_kbps;
    }
    if (queue_item) {
        // Queue items share the queue budget; the per-item limiter is kept in
        // the row (for a later move back to Downloads) but never used here.
        options.limit_enabled = false;
        options.limit_kbps = 0;
        // The queue's connection rule always wins; 0 there means global Settings.
        options.connections = queue ? queue->connections : 0;
    } else {
        // The per-item values are authoritative after live edits, including when
        // an older Options snapshot was supplied by the resume action.
        options.limit_enabled = item->bandwidth_limit_enabled;
        options.limit_kbps = item->bandwidth_limit_kbps;
        options.connections = item->connections;
    }
    if (options.output_dir.isEmpty()) {
        options.output_dir = default_download_location(
            QString::fromStdString(settings_.download_dir), settings_.category_dirs,
            options.category, settings_.category_folders);
    }
    // The GUI always resolves the folder (category included) before the task
    // starts, so the engine must not append a category subfolder again.
    options.apply_category_folder = false;
    // Fresh row id on every start: the per-download log follows the history row.
    options.history_id = item->history_id;

    auto* task = new DownloadTask(settings_, options, model_);
    if (probe) task->set_cached_probe(*probe);
    item->task = task;
    if (queue_item) {
        // Include this not-yet-started task in the share so it does not start
        // with a burst before the scheduler recomputes the split.
        task->set_transfer_limit(queue_share_for_new_task(queue_id));
    } else {
        item->bandwidth_limit_enabled = options.limit_enabled;
        item->bandwidth_limit_kbps = options.limit_kbps;
    }
    item->state = DownloadItem::State::Downloading;
    item->file_missing = false;
    item->last_try = now_stamp();
    item->last_try_epoch = swiftdm::unix_now();
    if (item->history_id > 0 && history_.is_open()) {
        history_.update(item->history_id, history_record_for(*item, &options));
    }
    wire_task(task);

    model_->touch_row(row);
    task->start();
    if (queue_item) apply_queue_speed_limits(queue_id);
    update_status_bar();
}

void MainWindow::wire_task(DownloadTask* task) {
    connect(task, &DownloadTask::probed, this, [this, task] {
        model_->task_probed(task);
        update_status_bar();
    });
    connect(task, &DownloadTask::progressed, this, [this, task] {
        model_->task_progressed(task);
        persist_progress(task);
    });
    connect(task, &DownloadTask::finished, this,
            [this, task](int status, const QString& path, const QString& error) {
                on_task_finished(task, status, path, error);
            });
}

void MainWindow::persist_progress(DownloadTask* task) {
    const int row = model_->row_for_task(task);
    if (row < 0) return;
    DownloadItem* item = model_->item_at(row);
    if (!item || item->history_id <= 0 || !history_.is_open()) return;
    // The engine reports several times per second; the DB only needs a
    // checkpoint every few seconds (a hard kill loses at most this window).
    const qint64 now = now_ms();
    const qint64 last = last_progress_save_ms_.value(item->history_id, 0);
    if (now - last < 5000) return;
    last_progress_save_ms_.insert(item->history_id, now);
    history_.update_progress(item->history_id, item->done, item->size);
}

void MainWindow::on_task_finished(DownloadTask* task, int status, const QString& path,
                                  const QString& error) {
    if (shutting_down_) {
        model_->task_finished(task, status, path, error);
        update_status_bar();
        return;
    }
    // Remember the row identity before the model removes canceled rows.
    const int before_row = model_->row_for_task(task);
    int item_id = 0;
    qint64 queue_id = 0;
    if (const DownloadItem* before = model_->item_at(before_row)) {
        item_id = before->id;
        queue_id = before->queue_id;
    }
    model_->task_finished(task, status, path, error);
    const int row = model_->row_for_task(task);
    if (row >= 0) {
        DownloadItem* item = model_->item_at(row);
        if (item) {
            if (queue_id > 0 && status == static_cast<int>(DownloadTask::Status::Paused) &&
                limit_paused_items_.remove(item_id)) {
                // The limit (not the user) paused this row. Put it back in the
                // queue when its workspace is still running; the scheduler will
                // start it again once a slot frees up.
                item->state = running_queues_.contains(queue_id)
                                  ? DownloadItem::State::Queued
                                  : DownloadItem::State::Paused;
                item->error.clear();
            } else if (status != static_cast<int>(DownloadTask::Status::Paused)) {
                limit_paused_items_.remove(item_id);
            }
            item->last_try_epoch = swiftdm::unix_now();
            if (!task->workspace_dir().isEmpty()) item->temp_path = task->workspace_dir();
            if (status == static_cast<int>(DownloadTask::Status::Completed)) {
                item->file_missing = path.isEmpty() || !QFileInfo::exists(path);
                item->temp_path.clear();
            }
        }
        if (item && item->history_id > 0 && history_.is_open()) {
            last_progress_save_ms_.remove(item->history_id);
            history_.update(item->history_id,
                            history_record_for(*item, item->task ? &item->task->options()
                                                                 : nullptr));
        }
        if (status == static_cast<int>(DownloadTask::Status::Completed)) {
            qInfo() << "download completed:" << path;
            // A queue item completes under the queue's own rules: the global
            // completion dialog and open-folder action belong to the main list.
            if (uses_global_completion_actions(queue_id)) {
                if (settings_.completion_dialog && item) {
                    show_completion_dialog(item->file_name, path);
                }
                if (settings_.open_folder_after_completion && !path.isEmpty()) {
                    open_download_folder(path);
                }
            }
            if (item && queue_id > 0) {
                const swiftdm::HistoryStore::QueueRecord* queue = queue_for(queue_id);
                if (queue && queue->notify_item) {
                    notify_queue(queue_id, tr("Download Complete"),
                                 tr("Queue \"%1\": %2 completed.")
                                     .arg(QString::fromStdString(queue->name),
                                          item->file_name));
                }
            }
        } else if (status == static_cast<int>(DownloadTask::Status::Failed) && item) {
            show_error_dialog(item->url, error, item->page_url);
        }
        if (queue_id > 0) {
            apply_queue_speed_limits(queue_id);
            if (running_queues_.contains(queue_id)) schedule_queue(queue_id);
            check_queue_completion(queue_id);
        }
    } else {
        // The row was removed while the task was running; clean up leftovers.
        if (!task->workspace_dir().isEmpty()) {
            QDir(task->workspace_dir()).removeRecursively();
        }
        task->deleteLater();
        limit_paused_items_.remove(item_id);
        if (queue_id > 0 && queue_for(queue_id)) {
            apply_queue_speed_limits(queue_id);
            if (running_queues_.contains(queue_id)) schedule_queue(queue_id);
            check_queue_completion(queue_id);
        }
    }
    update_status_bar();
}

void MainWindow::apply_refresh_address(int row, const QJsonObject& capture) {
    DownloadItem* item = model_->item_at(row);
    if (!item) return;

    const QString new_url = capture.value(QStringLiteral("url")).toString();
    if (new_url.isEmpty()) return;

    // ------------------------------------------------------------------
    // 1. Capture the old workspace before anything changes: the engine keys the
    //    workspace on `url + "\n" + filename`, so a new URL points at a new
    //    (empty) folder and the saved bytes would be lost.
    // ------------------------------------------------------------------
    const QString old_workspace = item->task ? item->task->workspace_dir() : QString();
    const QString old_workspace_temp = item->temp_path;

    // ------------------------------------------------------------------
    // 2. Build the new options: URL, cookies, referer, UA and headers all come
    //    from the fresh capture (the old ones are expired).
    // ------------------------------------------------------------------
    DownloadTask::Options options =
        item->task ? item->task->options() : DownloadTask::Options{};
    options.url = new_url;
    options.cookies = cookies_from_json(capture.value(QStringLiteral("cookies")));
    options.referer = capture.value(QStringLiteral("referer")).toString();
    const QString ua = capture.value(QStringLiteral("userAgent")).toString();
    if (!ua.isEmpty()) options.user_agent = ua;
    options.post_data = decode_post_data(capture);
    const auto new_headers = headers_from_json(capture.value(QStringLiteral("headers")));
    if (!new_headers.empty()) options.headers = new_headers;
    if (options.output_dir.isEmpty()) options.output_dir = item->output_dir;
    if (options.category.isEmpty()) options.category = item->category;

    // ------------------------------------------------------------------
    // 3. Move the workspace so the engine finds segments.json and the .part
    //    file under the new key.
    // ------------------------------------------------------------------
    QString moved_workspace;    // Compute the destination directory the engine will use for the new URL.
    // The engine keys on the *effective* URL, unknown until it probes, so the
    // safest move is a rename performed by matching the filename below. We
    // instead rewrite the state file URL and rename to the predictable key.
    const auto workspace_parent_of = [this](const QString& workspace) -> QString {
        if (workspace.isEmpty()) return QString();
        const int slash = workspace.lastIndexOf(QLatin1Char('/'));
        if (slash <= 0) return QString();
        return workspace.left(slash);
    };

    if (!old_workspace.isEmpty() && QDir(old_workspace).exists()) {
        const QString parent = workspace_parent_of(old_workspace);
        const QString new_key =
            new_url + QLatin1Char('\n') + item->file_name;
        const QString new_name = QStringLiteral("download-") +
            QString::fromStdString(swiftdm::hex_hash64(
                swiftdm::fnv1a_hash(new_key.toStdString())));
        const QString target = parent + QLatin1Char('/') + new_name;
        if (QDir().exists(old_workspace) && target != old_workspace) {
            if (QDir(target).exists()) swiftdm::remove_tree(target.toStdString());
            if (QDir().rename(old_workspace, target)) {
                moved_workspace = target;
                // Neutralise the saved validators: the new URL is the same file
                // but a different server resource identity. The engine already
                // tolerates a mismatch when the size matches, and the size is
                // the real guard here.
                rewrite_resume_state(target, new_url);
            } else {
                qWarning() << "refresh address: could not move workspace"
                           << old_workspace << "->" << target;
            }
        }
    }

    // ------------------------------------------------------------------
    // 4. Update the model row and the history record in place.
    // ------------------------------------------------------------------
    item->url = new_url;
    item->waiting_refresh = false;
    if (!moved_workspace.isEmpty()) item->temp_path = moved_workspace;
    else if (!old_workspace_temp.isEmpty()) item->temp_path = old_workspace_temp;
    const QString new_page = capture.value(QStringLiteral("pageUrl")).toString();
    if (!new_page.isEmpty()) item->page_url = new_page;
    const QString new_title = capture.value(QStringLiteral("pageTitle")).toString();
    if (!new_title.isEmpty()) item->page_title = new_title;

    pending_refresh_id_ = 0;
    hide_refresh_waiting_dialog();
    // Remember the applied address so the extension's retry of this same handoff
    // is dropped instead of opening another confirmation window.
    refresh_applied_url_ = new_url;
    refresh_applied_at_ms_ = QDateTime::currentMSecsSinceEpoch();

    if (item->history_id > 0 && history_.is_open()) {
        history_.update(item->history_id, history_record_for(*item, &options));
    }

    // The old task holds the expired URL; drop it so start_task_at_row builds a
    // fresh one from `options`.
    if (item->task) {
        DownloadTask* old = item->task;
        item->task = nullptr;
        old->deleteLater();
    }

    model_->touch_row(row);
    qInfo() << "refresh address: resuming" << item->file_name << "with" << new_url;

    // ------------------------------------------------------------------
    // 5. Restart the transfer: it resumes from the existing bytes because the
    //    segments.json moved with the workspace.
    // ------------------------------------------------------------------
    start_task_at_row(row, &options);
    open_download_window(item->id);
}


}  // namespace swiftdm::ui
