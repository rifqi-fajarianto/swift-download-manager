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
#include "ui/settings_dialog.h"
#include "ui/theme.h"

#include "ui/main_window_internal.h"

namespace swiftdm::ui {

QString now_stamp() {
    return QDateTime::currentDateTime().toString(QStringLiteral("MMM d HH:mm:ss"));
}

QString stamp_from_unix(int64_t seconds) {
    if (seconds <= 0) return {};
    return QDateTime::fromSecsSinceEpoch(seconds).toString(QStringLiteral("MMM d HH:mm:ss"));
}

qint64 now_ms() {
    return QDateTime::currentMSecsSinceEpoch();
}

const char* history_status(DownloadItem::State state) {
    switch (state) {
        case DownloadItem::State::Queued: return "Queued";
        case DownloadItem::State::Probing: return "Downloading";
        case DownloadItem::State::Downloading: return "Downloading";
        case DownloadItem::State::Paused: return "Paused";
        case DownloadItem::State::Complete: return "Complete";
        case DownloadItem::State::Error: return "Error";
    }
    return "Downloading";
}

// Builds the SQLite history row for an item. Browser page metadata, POST data
// and forwarded headers come from task options; live speed-limit values come
// from the item so an active edit is persisted even if options are stale.
swiftdm::HistoryStore::Record history_record_for(const DownloadItem& item,
                                                 const DownloadTask::Options* options) {
    swiftdm::HistoryStore::Record record;
    record.url = item.url.toStdString();
    record.filename = item.file_name.toStdString();
    record.category = item.category.toStdString();
    record.status = history_status(item.state);
    record.page_url = item.page_url.toStdString();
    record.page_title = item.page_title.toStdString();
    record.group_id = item.group.toStdString();
    record.error_text = item.error.toStdString();
    record.file_size = item.size;
    record.done = item.done;
    record.resumable = item.resumable;
    record.temp_path = item.temp_path.toStdString();
    record.bandwidth_limit_enabled = item.bandwidth_limit_enabled;
    record.bandwidth_limit_kbps = item.bandwidth_limit_kbps;
    record.connections = item.connections;
    record.queue_id = item.queue_id;
    record.last_try = item.last_try_epoch;
    record.folder_path = item.dest_path.isEmpty()
                             ? item.output_dir.toStdString()
                             : QFileInfo(item.dest_path).absolutePath().toStdString();
    // The list now stores the exact folder: the confirmation dialog resolves
    // the category subfolder before the item exists.
    record.folder_exact = true;
    // The saved name can differ from the requested one ("name (1).ext"), so a
    // completed download stores the name the file actually has on disk.
    if (item.is_complete() && !item.dest_path.isEmpty()) {
        const QString saved_name = QFileInfo(item.dest_path).fileName();
        if (!saved_name.isEmpty()) record.filename = saved_name.toStdString();
    }
    if (options) {
        record.user_agent = options->user_agent.toStdString();
        record.post_data.assign(options->post_data.constData(),
                                static_cast<size_t>(options->post_data.size()));
        if (!options->post_data.isEmpty()) record.method = "POST";
        for (const swiftdm::HttpHeader& header : options->headers) {
            record.headers.push_back(header.name + ": " + header.value);
        }
    }
    return record;
}

// History "Downloading" rows are stale after a restart: no transfer runs.
// Queue rows restore as Queued so Start Queue (or the autostart option) can
// pick them up; a main-list row restores as Paused like before.
DownloadItem::State state_from_record(const swiftdm::HistoryStore::Record& record) {
    if (record.status == "Complete") return DownloadItem::State::Complete;
    if (record.status == "Paused") return DownloadItem::State::Paused;
    if (record.status == "Error") return DownloadItem::State::Error;
    if (record.status == "Queued") {
        return record.queue_id > 0 ? DownloadItem::State::Queued
                                   : DownloadItem::State::Paused;
    }
    if (record.queue_id > 0) return DownloadItem::State::Queued;
    return DownloadItem::State::Paused;
}

// Older rows have no temppath column value. The workspace directory name is a
// hash of the effective URL and filename, so it can only be found by looking at
// the candidates. Match the URL recorded inside segments.json only: guessing
// from a same-named .part file could point Delete at another download.
QString find_existing_workspace(const swiftdm::Settings& settings,
                                const swiftdm::HistoryStore::Record& record) {
    if (record.url.empty()) return {};
    std::error_code ec;
    const std::filesystem::path root(settings.temp_dir);
    if (!std::filesystem::is_directory(root, ec)) return {};
    for (std::filesystem::directory_iterator it(root, ec), end; !ec && it != end;
         it.increment(ec)) {
        std::error_code type_ec;
        if (!it->is_directory(type_ec)) continue;
        const std::string name = it->path().filename().string();
        if (!swiftdm::starts_with(name, "download-")) continue;
        swiftdm::TransferState state;
        if (swiftdm::load_transfer_state((it->path() / "segments.json").string(), state,
                                         nullptr) &&
            state.url == record.url) {
            return QString::fromStdString(it->path().string());
        }
    }
    return {};
}

void MainWindow::restore_history() {
    if (!history_.is_open()) return;
    std::vector<swiftdm::HistoryStore::Entry> entries;
    std::string error;
    if (!history_.load_all(&entries, &error)) {
        qWarning() << "history: cannot restore list:" << QString::fromStdString(error);
        return;
    }

    int missing_count = 0;
    for (swiftdm::HistoryStore::Entry& entry : entries) {
        swiftdm::HistoryStore::Record& record = entry.record;
        // "Canceled" was a removal request in older builds; do not resurrect it.
        if (record.status == "Canceled") {
            history_.remove(entry.id);
            continue;
        }
        DownloadItem item;
        item.history_id = entry.id;
        item.url = QString::fromStdString(record.url);
        item.file_name = QString::fromStdString(record.filename);
        if (item.file_name.isEmpty()) {
            item.file_name =
                QString::fromStdString(swiftdm::file_name_from_url(record.url));
        }
        item.category = QString::fromStdString(record.category);
        if (item.category.isEmpty()) {
            item.category = QString::fromStdString(
                swiftdm::category_folder_for(item.file_name.toStdString()));
            // Keep the row (and the options rebuilt from it) consistent with
            // the category derived from the file name.
            record.category = item.category.toStdString();
        }
        item.output_dir = QString::fromStdString(record.folder_path);
        if (item.output_dir.isEmpty()) {
            item.output_dir = QString::fromStdString(settings_.download_dir);
            if (settings_.category_folders && !record.category.empty()) {
                item.output_dir += QLatin1Char('/') + QString::fromStdString(record.category);
            }
        }
        item.size = record.file_size;
        item.done = record.done;
        item.percent = record.file_size > 0
                           ? swiftdm::percent_of(record.done, record.file_size)
                           : 0;
        item.state = state_from_record(record);
        item.last_try_epoch = record.last_try;
        item.last_try = stamp_from_unix(record.last_try);
        item.added = stamp_from_unix(record.first_try);
        item.error = QString::fromStdString(record.error_text);
        item.page_url = QString::fromStdString(record.page_url);
        item.page_title = QString::fromStdString(record.page_title);
        item.group = QString::fromStdString(record.group_id);
        item.queue_id = record.queue_id;
        item.connections = record.connections;
        item.resumable = record.resumable;
        item.temp_path = QString::fromStdString(record.temp_path);
        item.bandwidth_limit_enabled = record.bandwidth_limit_enabled;
        item.bandwidth_limit_kbps = record.bandwidth_limit_kbps;

        if (item.state == DownloadItem::State::Complete) {
            item.done = item.size > 0 ? item.size : item.done;
            item.percent = 100;
            // The row stores the requested name; the file may have been saved
            // with a unique suffix, so resolve it against the folder.
            std::string folder = record.folder_path;
            if (folder.empty()) {
                folder = settings_.download_dir;
                if (settings_.category_folders && !record.category.empty()) {
                    folder += "/" + record.category;
                }
            }
            const std::string resolved = swiftdm::resolve_existing_file(
                folder, item.file_name.toStdString(), record.last_try);
            item.dest_path = QString::fromStdString(resolved);
            item.file_missing = resolved.empty();
            if (item.file_missing) {
                item.dest_path =
                    QDir(QString::fromStdString(folder)).filePath(item.file_name);
                ++missing_count;
            } else {
                item.file_name = QFileInfo(item.dest_path).fileName();
                if (item.size == 0) {
                    item.size = static_cast<quint64>(QFileInfo(item.dest_path).size());
                    item.done = item.size;
                }
            }
        } else {
            // Keep the temp workspace for the Properties dialog and resume.
            if (item.temp_path.isEmpty()) {
                item.temp_path = find_existing_workspace(settings_, record);
            }
            // The segments file knows how much data is really on disk (the DB
            // checkpoint can be a few seconds behind after a hard kill).
            if (!item.temp_path.isEmpty()) {
                swiftdm::TransferState state;
                const std::string state_path =
                    QDir(item.temp_path)
                        .filePath(QStringLiteral("segments.json"))
                        .toStdString();
                if (swiftdm::load_transfer_state(state_path, state, nullptr) &&
                    state.file_size > 0) {
                    item.done = state.done_bytes();
                    item.size = state.file_size;
                    item.percent = swiftdm::percent_of(item.done, item.size);
                }
            }
            // Restored rows are never started automatically; the user resumes.
            const DownloadTask::Options options = options_from_record(record);
            // Rows written before `folderexact` stored the base folder; the
            // rebuilt options carry the exact folder (category included).
            item.output_dir = options.output_dir;
            auto* task = new DownloadTask(settings_, options, model_);
            task->restore_progress(item.done, item.size, record.resumable);
            task->set_workspace_dir(item.temp_path);
            item.task = task;
            wire_task(task);
            const std::string restored_status = history_status(item.state);
            if (record.status != restored_status) {
                // A row left "Downloading"/"Queued" by a quit or crash. Write the
                // full row so lasttry (sort order) stays unchanged.
                record.status = restored_status;
                record.done = item.done;
                record.file_size = item.size;
                history_.update(entry.id, record);
            }
        }
        model_->add_item(item);
    }
    if (queues_root_) rebuild_queue_tree();
    qInfo() << "history: restored" << model_->rowCount() << "row(s),"
            << missing_count << "missing file(s)";
}

DownloadTask::Options MainWindow::options_from_record(
    const swiftdm::HistoryStore::Record& record) const {
    DownloadTask::Options options;
    options.url = QString::fromStdString(record.url);
    QString folder = QString::fromStdString(record.folder_path);
    if (folder.isEmpty()) folder = QString::fromStdString(settings_.download_dir);
    // Rows written before the `folderexact` column stored the base folder
    // only; their category subfolder still has to be appended on restore.
    if (!record.folder_exact && settings_.category_folders && !record.category.empty()) {
        const QString suffix = QLatin1Char('/') + QString::fromStdString(record.category);
        if (!folder.endsWith(suffix)) folder += suffix;
    }
    options.output_dir = QDir::cleanPath(folder);
    // The rebuilt folder already includes the category: the engine must not
    // append it a second time.
    options.apply_category_folder = false;
    options.category = QString::fromStdString(record.category);
    options.filename = QString::fromStdString(record.filename);
    options.user_agent = QString::fromStdString(record.user_agent);
    options.connections = record.connections;
    options.limit_enabled = record.bandwidth_limit_enabled;
    options.limit_kbps = record.bandwidth_limit_kbps;
    options.post_data = QByteArray(record.post_data.data(),
                                   static_cast<int>(record.post_data.size()));
    for (const std::string& line : record.headers) {
        const size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        swiftdm::HttpHeader header;
        header.name = swiftdm::trim(line.substr(0, colon));
        header.value = swiftdm::trim(line.substr(colon + 1));
        if (!header.name.empty()) options.headers.push_back(std::move(header));
    }
    return options;
}

}  // namespace swiftdm::ui
