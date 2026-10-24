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

namespace swiftdm::ui {

namespace {

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

struct CategoryEntry {
    const char* label;
    const char* icon;
};

const CategoryEntry kCategories[] = {
    {"Video", ":/icons/video.svg"},           {"Audio", ":/icons/music.svg"},
    {"Compressed", ":/icons/compressed.svg"}, {"Document", ":/icons/document.svg"},
    {"Application", ":/icons/applications.svg"},
    {"Misc", ":/icons/other.svg"},
};

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

}  // namespace

MainWindow::MainWindow(swiftdm::Settings settings, QWidget* parent)
    : QMainWindow(parent), settings_(std::move(settings)) {
    setWindowTitle(QStringLiteral("SwiftDM %1").arg(swiftdm::kVersionString));
    resize(1020, 580);

    std::string history_error;
    if (!history_.open(swiftdm::default_database_path(), &history_error)) {
        qWarning() << "history: persistence disabled:"
                   << QString::fromStdString(history_error);
    }
    load_queues();

    model_ = new DownloadModel(this);
    proxy_ = new DownloadFilterProxy(this);
    proxy_->setSourceModel(model_);

    build_actions();
    build_body();

    shutdown_timer_ = new QTimer(this);
    shutdown_timer_->setInterval(1000);
    connect(shutdown_timer_, &QTimer::timeout, this, &MainWindow::on_shutdown_tick);
    browser_integration_ = new BrowserIntegration(this);
    connect(browser_integration_, &BrowserIntegration::downloadRequested, this,
            &MainWindow::add_browser_download);
    connect(browser_integration_, &BrowserIntegration::showPanelRequested, this,
            [this](bool visible) {
                if (visible) {
                    showNormal();
                    raise();
                    activateWindow();
                } else {
                    hide();
                }
            });
    connect(browser_integration_, &BrowserIntegration::serverError, this,
            [this](const QString& error) {
                statusBar()->showMessage(tr("Browser integration unavailable: %1").arg(error));
            });
    connect(browser_integration_, &BrowserIntegration::handshakeRejected, this,
            [this](const QString& reason) {
                statusBar()->showMessage(
                    tr("Browser extension connection rejected: %1").arg(reason), 10000);
            });
    build_tray();

#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    // A "follow system" theme follows the desktop when the color scheme
    // changes while the app runs.
    if (const QStyleHints* hints = QGuiApplication::styleHints()) {
        connect(hints, &QStyleHints::colorSchemeChanged, this, [this] {
            if (settings_.theme != "system") return;
            apply_interface_settings();
        });
    }
#endif

    connect(model_, &QAbstractItemModel::rowsInserted, this,
            [this] { update_status_bar(); });
    connect(model_, &QAbstractItemModel::rowsRemoved, this,
            [this] { update_status_bar(); });
    connect(model_, &QAbstractItemModel::dataChanged, this,
            [this] { update_status_bar(); });
    restore_history();
    update_status_bar();
    start_autostart_queues();
}

MainWindow::~MainWindow() {
    // A pending countdown must not outlive the window.
    shutdown_scheduler_.cancel_all();
    if (shutdown_timer_) shutdown_timer_->stop();
    // Pause every running transfer and wait for its worker so segments.json is
    // written before the process goes away; the list then restores as Paused
    // (queue items restore as Queued so their queue can pick them up again).
    for (int row = 0; row < model_->rowCount(); ++row) {
        DownloadItem* item = model_->item_at(row);
        if (!item || !item->task) continue;
        const bool was_active = item->task->active();
        item->task->pause_and_wait();
        if (was_active && (item->state == DownloadItem::State::Downloading ||
                           item->state == DownloadItem::State::Probing)) {
            item->state = item->queue_id > 0 ? DownloadItem::State::Queued
                                             : DownloadItem::State::Paused;
        }
        if (!item->task->workspace_dir().isEmpty()) {
            item->temp_path = item->task->workspace_dir();
        }
        // The worker's snapshot has the final byte count written by the engine
        // (the last progressed() signal may still be queued). Completed rows
        // already carry the full size, so do not lower their done count.
        const DownloadTask::Snapshot snapshot = item->task->snapshot();
        if (!item->is_complete() && snapshot.done > 0) {
            item->done = snapshot.done;
            if (snapshot.total > 0) item->size = snapshot.total;
            item->percent = item->size > 0
                                ? swiftdm::percent_of(item->done, item->size)
                                : 0;
        }
        if (item->history_id > 0 && history_.is_open()) {
            history_.update(item->history_id, history_record_for(*item, &item->task->options()));
        }
    }
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

void MainWindow::build_actions() {
    act_new_ = new QAction(tinted_icon(QStringLiteral(":/icons/link-plus.svg"), colors::kActionBlue),
                           tr("New URL"), this);
    act_resume_ =
        new QAction(tinted_icon(QStringLiteral(":/icons/resume-start.svg"), colors::kActionBlue),
                    tr("Resume"), this);
    act_stop_ = new QAction(tinted_icon(QStringLiteral(":/icons/stop.svg"), colors::kActionBlue),
                            tr("Stop"), this);
    act_delete_ = new QAction(tinted_icon(QStringLiteral(":/icons/delete.svg"), colors::kDanger),
                              tr("Delete"), this);
    act_delete_file_ =
        new QAction(tinted_icon(QStringLiteral(":/icons/delete-file.svg"), colors::kDanger),
                    tr("Delete File"), this);
    act_delete_file_->setToolTip(tr("Delete the downloaded file from disk"));
    act_settings_ =
        new QAction(tinted_icon(QStringLiteral(":/icons/settings.svg"), colors::neutral_tint()),
                    tr("Settings"), this);
    act_browsers_ =
        new QAction(tinted_icon(QStringLiteral(":/icons/browsers.svg"), colors::neutral_tint()),
                    tr("Extensions"), this);
    act_about_ = new QAction(tinted_icon(QStringLiteral(":/icons/about.svg"), colors::kActionBlue),
                             tr("About"), this);
    act_quit_ = new QAction(tinted_icon(QStringLiteral(":/icons/quit.svg"), colors::kDanger),
                            tr("Quit"), this);

    // The queue toolbar reuses the main labels ("New URL", "Delete",
    // "Delete File") but keeps its own action for "New URL" so the button can
    // move to the third group without disturbing the Downloads toolbar.
    act_queue_add_ =
        new QAction(tinted_icon(QStringLiteral(":/icons/link-plus.svg"), colors::kActionBlue),
                    tr("New URL"), this);
    act_queue_add_->setToolTip(tr("Add a new download to this queue"));
    act_start_queue_ =
        new QAction(tinted_icon(QStringLiteral(":/icons/resume-start.svg"), colors::kActionBlue),
                    tr("Start Queue"), this);
    act_start_queue_->setToolTip(tr("Resume every queued, paused and failed item in this queue"));
    act_pause_queue_ =
        new QAction(tinted_icon(QStringLiteral(":/icons/stop.svg"), colors::kActionBlue),
                    tr("Stop Queue"), this);
    act_pause_queue_->setToolTip(tr("Pause the active downloads of this queue"));
    act_queue_props_ =
        new QAction(tinted_icon(QStringLiteral(":/icons/queue-settings.svg"),
                                colors::neutral_tint()),
                    tr("Queue Properties"), this);
    act_new_queue_ =
        new QAction(tinted_icon(QStringLiteral(":/icons/queue-add.svg"), colors::kActionBlue),
                    tr("New Queue"), this);
    act_delete_queue_ =
        new QAction(tinted_icon(QStringLiteral(":/icons/queue-delete.svg"), colors::kDanger),
                    tr("Delete Queue"), this);
    act_delete_queue_->setToolTip(tr("Delete the selected queue and its items"));

    // Queue-only actions start disabled; the first update_action_states()
    // enables the ones that apply to the current view.
    act_queue_add_->setEnabled(false);
    act_delete_queue_->setEnabled(false);
    act_queue_props_->setEnabled(false);
    act_start_queue_->setEnabled(false);
    act_pause_queue_->setEnabled(false);
    act_new_queue_->setEnabled(queue_features_available());

    act_new_->setShortcut(QKeySequence::New);
    act_queue_add_->setShortcut(QKeySequence::New);
    act_delete_->setShortcut(QKeySequence::Delete);

    connect(act_new_, &QAction::triggered, this, &MainWindow::on_new_url);
    connect(act_queue_add_, &QAction::triggered, this, &MainWindow::on_new_url);
    connect(act_resume_, &QAction::triggered, this, &MainWindow::on_resume);
    connect(act_stop_, &QAction::triggered, this, &MainWindow::on_stop);
    connect(act_delete_, &QAction::triggered, this, &MainWindow::on_delete);
    connect(act_delete_file_, &QAction::triggered, this, &MainWindow::on_delete_file);
    connect(act_settings_, &QAction::triggered, this, &MainWindow::on_settings);
    connect(act_browsers_, &QAction::triggered, this, &MainWindow::on_browsers);
    connect(act_about_, &QAction::triggered, this, &MainWindow::on_about);
    connect(act_quit_, &QAction::triggered, this, &MainWindow::on_quit);
    connect(act_start_queue_, &QAction::triggered, this, &MainWindow::on_start_queue);
    connect(act_pause_queue_, &QAction::triggered, this, &MainWindow::on_pause_queue);
    connect(act_queue_props_, &QAction::triggered, this, &MainWindow::on_queue_properties);
    connect(act_new_queue_, &QAction::triggered, this, &MainWindow::on_new_queue);
    connect(act_delete_queue_, &QAction::triggered, this, &MainWindow::on_delete_queue);

    auto* toolbar = addToolBar(tr("Main"));
    toolbar_ = toolbar;
    toolbar->setObjectName(QStringLiteral("main_toolbar"));
    toolbar->setMovable(false);
    toolbar->addAction(act_new_);
    sep_after_new_ = toolbar->addSeparator();
    toolbar->addAction(act_resume_);
    toolbar->addAction(act_stop_);
    sep_download_actions_ = toolbar->addSeparator();
    toolbar->addAction(act_new_queue_);
    toolbar->addAction(act_delete_queue_);
    toolbar->addAction(act_queue_props_);
    sep_queue_actions_ = toolbar->addSeparator();
    toolbar->addAction(act_start_queue_);
    toolbar->addAction(act_pause_queue_);
    sep_queue_transfer_actions_ = toolbar->addSeparator();
    toolbar->addAction(act_queue_add_);
    toolbar->addAction(act_delete_);
    toolbar->addAction(act_delete_file_);
    auto* spacer = new QWidget(toolbar);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    toolbar->addWidget(spacer);
    toolbar->addAction(act_settings_);
    toolbar->addAction(act_browsers_);
    toolbar->addAction(act_about_);
    toolbar->addAction(act_quit_);
    apply_toolbar_settings();
    update_queue_action_visibility();
}

// The toolbar size and the text under the icons follow the Interface tab.
void MainWindow::apply_toolbar_settings() {
    if (!toolbar_) return;
    const bool small = settings_.toolbar_size == "small";
    const int icon_size = small ? 22 : 28;
    const bool with_text = settings_.show_toolbar_text;

    toolbar_->setIconSize(QSize(icon_size, icon_size));
    toolbar_->setToolButtonStyle(with_text ? Qt::ToolButtonTextUnderIcon
                                           : Qt::ToolButtonIconOnly);
    for (QAction* action : toolbar_->actions()) {
        auto* button = qobject_cast<QToolButton*>(toolbar_->widgetForAction(action));
        if (!button) continue;
        if (with_text) {
            button->setFixedSize(small ? 64 : 88, small ? 56 : 76);
        } else {
            // Without the text the button only holds the icon.
            const int side = icon_size + 12;
            button->setFixedSize(side, side);
        }
    }
}

// The tray icon style follows the Interface tab.
void MainWindow::apply_tray_settings() {
    if (!tray_) return;
    const TrayIconStyle style = tray_icon_style_from_string(
        QString::fromStdString(settings_.tray_icon_style));
    tray_->setIcon(tray_icon(style));
}

// The neutral tint depends on the theme, so the icons are re-tinted whenever
// the theme changes.
void MainWindow::refresh_action_icons() {
    act_settings_->setIcon(
        tinted_icon(QStringLiteral(":/icons/settings.svg"), colors::neutral_tint()));
    act_browsers_->setIcon(
        tinted_icon(QStringLiteral(":/icons/browsers.svg"), colors::neutral_tint()));
    act_queue_props_->setIcon(
        tinted_icon(QStringLiteral(":/icons/queue-settings.svg"), colors::neutral_tint()));
    act_new_queue_->setIcon(
        tinted_icon(QStringLiteral(":/icons/queue-add.svg"), colors::kActionBlue));
    act_delete_queue_->setIcon(
        tinted_icon(QStringLiteral(":/icons/queue-delete.svg"), colors::kDanger));
    act_queue_add_->setIcon(
        tinted_icon(QStringLiteral(":/icons/link-plus.svg"), colors::kActionBlue));
}

// Applies the appearance settings without a restart.
void MainWindow::apply_interface_settings() {
    if (auto* app = qobject_cast<QApplication*>(QApplication::instance())) {
        apply_theme(*app, theme_mode_from_string(QString::fromStdString(settings_.theme)));
    }
    refresh_action_icons();
    refresh_queue_overview();
    apply_toolbar_settings();
    apply_tray_settings();
    // The palette change repaints the widgets, but the custom painted ones
    // (segment bar) have to be asked explicitly.
    for (QWidget* widget : QApplication::topLevelWidgets()) widget->update();
}

void MainWindow::build_body() {
    auto* splitter = new QSplitter(Qt::Horizontal, this);

    tree_ = new QTreeWidget(splitter);
    tree_->setHeaderHidden(true);
    tree_->setRootIsDecorated(true);
    tree_->setMinimumWidth(170);
    tree_->setUniformRowHeights(true);

    auto* all = new QTreeWidgetItem(tree_, {QStringLiteral("All Downloads")});
    all->setData(0, Qt::UserRole, QString());
    all->setIcon(0, tinted_icon(QStringLiteral(":/icons/folder.svg"), colors::kActionBlue));
    all_item_ = all;
    for (const CategoryEntry& entry : kCategories) {
        auto* child = new QTreeWidgetItem(all, {QString::fromLatin1(entry.label)});
        child->setData(0, Qt::UserRole, QString::fromLatin1(entry.label));
        child->setIcon(0, tinted_icon(QString::fromLatin1(entry.icon), colors::kActionBlue));
    }
    all->setExpanded(true);

    struct ViewEntry {
        const char* label;
        const char* key;
        const char* icon;
    };
    for (const ViewEntry& entry :
         {ViewEntry{"Complete", "__complete", ":/icons/complete.svg"},
          ViewEntry{"Incomplete", "__incomplete", ":/icons/alert.svg"}}) {
        auto* item = new QTreeWidgetItem(tree_, {QString::fromLatin1(entry.label)});
        item->setData(0, Qt::UserRole, QString::fromLatin1(entry.key));
        item->setIcon(0, tinted_icon(QString::fromLatin1(entry.icon), colors::kActionBlue));
    }

    // Queues are a separate workspace, so they live in their own sidebar
    // branch instead of being another status on the main list. The branch is
    // selectable and opens the queue overview in the right panel.
    queues_root_ = new QTreeWidgetItem(tree_, {tr("Queues")});
    queues_root_->setData(0, Qt::UserRole, QStringLiteral("__queues"));
    queues_root_->setIcon(0, tinted_icon(QStringLiteral(":/icons/queue.svg"),
                                         colors::kActionBlue));
    rebuild_queue_tree();
    queues_root_->setExpanded(true);

    tree_->setCurrentItem(all);
    connect(tree_, &QTreeWidget::itemSelectionChanged, this, &MainWindow::on_tree_selection);
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(tree_, &QTreeWidget::customContextMenuRequested, this,
            &MainWindow::on_tree_context_menu);

    right_stack_ = new QStackedWidget(splitter);
    table_ = new QTableView(right_stack_);
    table_->setModel(proxy_);
    table_->setSortingEnabled(true);
    proxy_->setSortRole(Qt::UserRole);  // Last Try sorts by epoch, not text
    table_->sortByColumn(DownloadModel::ColLastTry, Qt::DescendingOrder);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setWordWrap(false);
    table_->verticalHeader()->setVisible(false);
    table_->verticalHeader()->setDefaultSectionSize(22);
    table_->horizontalHeader()->setHighlightSections(false);
    table_->horizontalHeader()->setSectionResizeMode(DownloadModel::ColName,
                                                     QHeaderView::Stretch);
    for (const int column : {DownloadModel::ColSize, DownloadModel::ColStatus,
                             DownloadModel::ColBandwidth, DownloadModel::ColRemaining,
                             DownloadModel::ColLastTry}) {
        table_->horizontalHeader()->setSectionResizeMode(column,
                                                         QHeaderView::ResizeToContents);
    }
    connect(table_, &QTableView::doubleClicked, this, &MainWindow::on_row_activated);
    table_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(table_, &QTableView::customContextMenuRequested, this, &MainWindow::on_context_menu);
    connect(table_->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            [this] { update_action_states(); });

    // The right panel swaps between the download table and a queue overview.
    // The overview lists the queues themselves (name + item count); it never
    // shows every queue item in one table.
    queue_overview_ = new QWidget(right_stack_);
    queue_overview_->setObjectName(QStringLiteral("queue_overview"));
    auto* overview_layout = new QVBoxLayout(queue_overview_);
    overview_layout->setContentsMargins(16, 16, 16, 16);
    overview_layout->setSpacing(10);
    queue_title_ = new QLabel(tr("Queues"), queue_overview_);
    queue_title_->setObjectName(QStringLiteral("queue_overview_title"));
    QFont title_font = queue_title_->font();
    title_font.setBold(true);
    title_font.setPointSizeF(title_font.pointSizeF() + 2.0);
    queue_title_->setFont(title_font);
    overview_layout->addWidget(queue_title_);

    queue_pages_ = new QStackedWidget(queue_overview_);
    queue_pages_->setObjectName(QStringLiteral("queue_overview_pages"));

    auto* list_page = new QWidget(queue_pages_);
    auto* list_layout = new QVBoxLayout(list_page);
    list_layout->setContentsMargins(0, 0, 0, 0);
    auto* list_hint = new QLabel(tr("Select a queue to open its downloads."), list_page);
    list_hint->setObjectName(QStringLiteral("queue_overview_hint"));
    list_hint->setStyleSheet(QStringLiteral("color:#a0a0a0;"));
    list_layout->addWidget(list_hint);
    queue_list_ = new QListWidget(list_page);
    queue_list_->setObjectName(QStringLiteral("queue_overview_list"));
    queue_list_->setIconSize(QSize(24, 24));
    queue_list_->setSelectionMode(QAbstractItemView::SingleSelection);
    list_layout->addWidget(queue_list_, 1);
    connect(queue_list_, &QListWidget::itemClicked, this,
            &MainWindow::on_queue_overview_clicked);
    queue_pages_->addWidget(list_page);

    auto* empty_page = new QWidget(queue_pages_);
    auto* empty_layout = new QVBoxLayout(empty_page);
    empty_layout->addStretch(1);
    queue_empty_label_ = new QLabel(empty_page);
    queue_empty_label_->setObjectName(QStringLiteral("queue_overview_empty"));
    queue_empty_label_->setAlignment(Qt::AlignCenter);
    queue_empty_label_->setWordWrap(true);
    empty_layout->addWidget(queue_empty_label_);
    queue_empty_button_ =
        new QPushButton(tinted_icon(QStringLiteral(":/icons/queue-add.svg"),
                                    colors::kActionBlue),
                        tr("New Queue"), empty_page);
    queue_empty_button_->setObjectName(QStringLiteral("queue_overview_new"));
    connect(queue_empty_button_, &QPushButton::clicked, this, &MainWindow::on_new_queue);
    empty_layout->addWidget(queue_empty_button_, 0, Qt::AlignCenter);
    empty_layout->addStretch(1);
    queue_pages_->addWidget(empty_page);

    overview_layout->addWidget(queue_pages_, 1);
    right_stack_->addWidget(table_);
    right_stack_->addWidget(queue_overview_);
    right_stack_->setCurrentWidget(table_);
    refresh_queue_overview();

    splitter->addWidget(tree_);
    splitter->addWidget(right_stack_);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    setCentralWidget(splitter);

    status_label_ = new QLabel(this);
    statusBar()->addWidget(status_label_);
}

void MainWindow::build_tray() {
    if (tray_) return;
    if (!settings_.show_tray_icon) return;
    if (!QSystemTrayIcon::isSystemTrayAvailable()) return;

    tray_ = new QSystemTrayIcon(this);
    apply_tray_settings();
    tray_->setToolTip(QStringLiteral("SwiftDM %1").arg(swiftdm::kVersionString));

    auto* menu = new QMenu(this);
    menu->addAction(tr("Show Main Window"), this, [this] {
        showNormal();
        raise();
        activateWindow();
    });
    menu->addSeparator();
    menu->addAction(tr("Quit"), this, &MainWindow::on_quit);
    tray_->setContextMenu(menu);

    connect(tray_, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason) {
                if (reason == QSystemTrayIcon::Trigger ||
                    reason == QSystemTrayIcon::DoubleClick) {
                    showNormal();
                    raise();
                    activateWindow();
                }
            });
    tray_->show();
}

// The tray icon can be switched on and off while the app runs.
void MainWindow::apply_tray_visibility() {
    if (settings_.show_tray_icon) {
        build_tray();
        return;
    }
    if (!tray_) return;
    tray_->hide();
    tray_->deleteLater();
    tray_ = nullptr;
    tray_note_shown_ = false;
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (!quitting_ && tray_ && settings_.close_to_tray) {
        event->ignore();
        hide();
        if (!tray_note_shown_) {
            tray_note_shown_ = true;
            tray_->showMessage(tr("SwiftDM"),
                               tr("SwiftDM keeps running in the notification area.\n"
                                  "Right-click the tray icon to quit."),
                               QSystemTrayIcon::Information, 4000);
        }
        return;
    }
    // Without a tray to hide into, closing the window ends the app. The same
    // confirmation as the Quit action is asked first.
    if (!quitting_ && !confirm_quit()) {
        event->ignore();
        show();
        return;
    }
    event->accept();
    quitting_ = true;
    // quitOnLastWindowClosed is disabled for the tray application, so close the
    // process explicitly when there is no tray icon to hide into.
    QApplication::quit();
}

// Asks whether the app really closes. The prompt is independent of the main
// window, so using Quit from the tray does not restore a hidden main window
// just to parent the dialog.
bool MainWindow::confirm_quit() {
    QMessageBox confirmation;
    confirmation.setIconPixmap(dialog_question_pixmap());
    confirmation.setWindowTitle(tr("Quit SwiftDM"));
    confirmation.setText(
        tr("SwiftDM will close completely. Active downloads will be paused and can be "
           "resumed the next time you start the app. Do you want to quit?"));
    QPushButton* quit_button = confirmation.addButton(tr("Quit"), QMessageBox::AcceptRole);
    auto* cancel_button = confirmation.addButton(QMessageBox::Cancel);
    confirmation.setDefaultButton(cancel_button);
    confirmation.exec();
    return confirmation.clickedButton() == quit_button;
}

void MainWindow::on_new_url() {
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
    const QJsonArray header_entries = data.value(QStringLiteral("headers")).toArray();
    for (const QJsonValue& entry : header_entries) {
        const QJsonObject header = entry.toObject();
        swiftdm::HttpHeader value;
        value.name = header.value(QStringLiteral("name")).toString().toStdString();
        value.value = header.value(QStringLiteral("value")).toString().toStdString();
        if (!value.name.empty()) options.headers.push_back(std::move(value));
    }
    const QString post_data = data.value(QStringLiteral("postData")).toString();
    if (!post_data.isEmpty()) {
        if (data.value(QStringLiteral("postDataEncoding")).toString() == QStringLiteral("base64")) {
            options.post_data = QByteArray::fromBase64(post_data.toLatin1());
        } else {
            options.post_data = post_data.toUtf8();
        }
    }
    const QJsonArray cookie_entries = data.value(QStringLiteral("cookies")).toArray();
    for (const QJsonValue& entry : cookie_entries) {
        const QJsonObject cookie = entry.toObject();
        swiftdm::HttpCookie value;
        value.domain = cookie.value(QStringLiteral("domain")).toString().toStdString();
        value.path = cookie.value(QStringLiteral("path")).toString().toStdString();
        value.name = cookie.value(QStringLiteral("name")).toString().toStdString();
        value.value = cookie.value(QStringLiteral("value")).toString().toStdString();
        value.secure = cookie.value(QStringLiteral("secure")).toBool();
        value.expires = static_cast<int64_t>(
            cookie.value(QStringLiteral("expires")).toDouble());
        options.cookies.push_back(std::move(value));
    }

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
            QString::fromStdString(settings_.download_dir), settings_.category_folders);
        dialog->set_queue_choices(queue_choices(), flow->item.queue_id);
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
                QString::fromStdString(settings_.download_dir), draft.category,
                settings_.category_folders);
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
        options.output_dir = QString::fromStdString(settings_.download_dir);
        if (settings_.category_folders && !options.category.isEmpty()) {
            options.output_dir += QLatin1Char('/') + options.category;
        }
    }
    // The GUI always resolves the folder (category included) before the task
    // starts, so the engine must not append a category subfolder again.
    options.apply_category_folder = false;

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
            if (settings_.completion_dialog && item) {
                show_completion_dialog(item->file_name, path);
            }
            if (settings_.open_folder_after_completion && !path.isEmpty()) {
                open_download_folder(path);
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

// Opens the folder that holds the downloaded file; used by the "open the
// download folder when a download completes" setting.
void MainWindow::open_download_folder(const QString& path) {
    const QFileInfo file(path);
    const QString dir = file.absolutePath();
    if (!QFileInfo(dir).isDir()) {
        qWarning() << "completion: the download folder is gone" << dir;
        return;
    }
    qInfo() << "completion: opening folder" << dir;
    QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
}

void MainWindow::show_completion_dialog(const QString& name, const QString& path) {    // Independent top-level window: no parent/transient hint, so the window
    // manager cannot group it with the main window when it is moved. Non-modal,
    // so the main window stays usable while the notification is on screen.
    auto* box = new QMessageBox(nullptr);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setWindowModality(Qt::NonModal);
    box->setWindowTitle(tr("Download Complete"));
    box->setIconPixmap(dialog_complete_pixmap());
    box->setText(QStringLiteral("%1\n%2").arg(name, path));
    QPushButton* open_folder = box->addButton(tr("Open Folder"), QMessageBox::ActionRole);
    QPushButton* open_file = box->addButton(tr("Open"), QMessageBox::ActionRole);
    box->addButton(QMessageBox::Close);
    // Connect directly to each button. QMessageBox closes (and with
    // WA_DeleteOnClose later deletes) itself when a button is clicked, so using
    // the dialog as the receiver for buttonClicked can skip the action.
    connect(open_folder, &QPushButton::clicked, this, [this, path] {
        const QFileInfo file(path);
        const QString dir = file.absolutePath();
        if (!QFileInfo(dir).isDir()) {
            show_error_dialog(dir, tr("The download folder no longer exists."));
            return;
        }
        qInfo() << "completion dialog: opening folder" << dir;
        QString folder_url = dir;
        if (!folder_url.endsWith(QLatin1Char('/'))) folder_url += QLatin1Char('/');
        const bool opened = QDesktopServices::openUrl(QUrl::fromLocalFile(folder_url));
        qInfo() << "completion dialog: openUrl returned" << opened;
        if (!opened && !QProcess::startDetached(QStringLiteral("xdg-open"), {dir})) {
            show_error_dialog(dir, tr("Could not open the download folder."));
        }
    });
    connect(open_file, &QPushButton::clicked, this, [this, path] {
        const QFileInfo file(path);
        if (!file.exists()) {
            show_error_dialog(path, tr("The downloaded file no longer exists."));
            return;
        }
        const QString target = file.absoluteFilePath();
        qInfo() << "completion dialog: opening file" << target;
        const bool opened = QDesktopServices::openUrl(QUrl::fromLocalFile(target));
        qInfo() << "completion dialog: openUrl returned" << opened;
        if (!opened && !QProcess::startDetached(QStringLiteral("xdg-open"), {target})) {
            show_error_dialog(target, tr("Could not open the downloaded file."));
        }
    });
    box->adjustSize();
    box->move(frameGeometry().center() - box->rect().center());
    box->show();
    box->raise();
    box->activateWindow();
}

void MainWindow::show_error_dialog(const QString& url, const QString& error,
                                   const QString& page_url) {
    // Same rationale as the completion window: an independent top-level window.
    auto* box = new QMessageBox(nullptr);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setWindowModality(Qt::NonModal);
    box->setWindowTitle(tr("Download Error"));
    box->setIconPixmap(dialog_warning_pixmap());
    box->setText(tr("Error : %1").arg(error));
    // A long URL must not widen the notification: show an elided form and keep
    // the complete URL behind "Show Details…".
    box->setInformativeText(QFontMetrics(box->font()).elidedText(url, Qt::ElideMiddle, 480));
    box->setDetailedText(url);
    // Hosts that demand a site visit or a captcha (pixeldrain's hotlink
    // protection, for example) say so in the error text. Offer the page that
    // started the download so the user can clear it and retry.
    if (!page_url.isEmpty()) {
        QPushButton* open_page =
            box->addButton(tr("Open Page in Browser"), QMessageBox::ActionRole);
        connect(open_page, &QPushButton::clicked, this, [page_url] {
            qInfo() << "error dialog: opening page" << page_url;
            QDesktopServices::openUrl(QUrl(page_url));
        });
    }
    box->addButton(QMessageBox::Close);
    box->adjustSize();
    box->move(frameGeometry().center() - box->rect().center());
    box->show();
    box->raise();
    box->activateWindow();
}

QList<int> MainWindow::selected_rows() const {
    QList<int> rows;
    const QModelIndexList indexes = table_->selectionModel()->selectedRows();
    for (const QModelIndex& index : indexes) {
        rows.append(proxy_->mapToSource(index).row());
    }
    std::sort(rows.begin(), rows.end(), std::greater<int>());
    return rows;
}

int MainWindow::row_at_view_pos(const QPoint& pos) const {
    const QModelIndex index = table_->indexAt(pos);
    if (!index.isValid()) return -1;
    return proxy_->mapToSource(index).row();
}

void MainWindow::on_context_menu(const QPoint& pos) {
    const int row = row_at_view_pos(pos);
    const DownloadItem* item = model_->item_at(row);
    if (!item) return;

    // Right-click selects the row under the cursor first (like the original,
    // which shows this menu on the list's NM_RCLICK notification).
    const QModelIndex index = proxy_->mapFromSource(model_->index(row, 0));
    if (!table_->selectionModel()->isSelected(index)) {
        table_->clearSelection();
        table_->selectionModel()->setCurrentIndex(
            index, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    }

    // The file can disappear while the app is running; re-check before the menu
    // decides whether Open File is available.
    refresh_missing_state(row);
    item = model_->item_at(row);
    if (!item) return;

    const QFileInfo file(item->dest_path);
    const bool file_ok = item->is_complete() && !item->dest_path.isEmpty() && file.exists();
    const bool complete_row = item->is_complete();
    const QString dir = item->dest_path.isEmpty() ? item->output_dir : file.absolutePath();
    const bool dir_ok = !dir.isEmpty() && QFileInfo(dir).isDir();

    QMenu menu(this);
    QAction* open_file = menu.addAction(
        tinted_icon(QStringLiteral(":/icons/document.svg"), colors::kNeutral), tr("Open File"));
    QAction* open_folder = menu.addAction(
        tinted_icon(QStringLiteral(":/icons/folder.svg"), colors::kNeutral),
        tr("Open File Location"));
    menu.addSeparator();

    const bool item_active = item->task && item->task->active();
    QAction* start_item = nullptr;
    QAction* pause_item = nullptr;
    QAction* move_to_downloads = nullptr;
    QMenu* move_menu = nullptr;
    QHash<QAction*, qint64> move_targets;
    if (!item->is_complete() || selected_queue_id_ > 0 || !item_active) {
        if (selected_queue_id_ > 0) {
            start_item = menu.addAction(
                tinted_icon(QStringLiteral(":/icons/resume-start.svg"), colors::kActionBlue),
                tr("Start"));
            start_item->setEnabled(!item_active && !item->is_complete());
            pause_item = menu.addAction(
                tinted_icon(QStringLiteral(":/icons/stop.svg"), colors::kActionBlue),
                tr("Pause"));
            pause_item->setEnabled(item_active);
            move_to_downloads = menu.addAction(
                tinted_icon(QStringLiteral(":/icons/folder.svg"), colors::kNeutral),
                tr("Move to Downloads"));
            move_to_downloads->setEnabled(!item_active);
        } else {
            move_menu = menu.addMenu(tr("Move to Queue"));
            move_menu->setEnabled(!item_active);
            for (const swiftdm::HistoryStore::QueueRecord& queue : queues_) {
                QAction* action =
                    move_menu->addAction(QString::fromStdString(queue.name));
                move_targets.insert(action, queue.id);
            }
            if (queues_.isEmpty()) {
                QAction* none = move_menu->addAction(tr("No queues"));
                none->setEnabled(false);
            }
        }
        menu.addSeparator();
    }

    QAction* properties = menu.addAction(
        tinted_icon(QStringLiteral(":/icons/properties.svg"), colors::kNeutral), tr("Properties"));
    // A completed row stays actionable even when the file is gone: the action
    // then explains that the file was deleted or moved.
    open_file->setEnabled(complete_row && !item->dest_path.isEmpty());
    open_folder->setEnabled(dir_ok);
    if (complete_row && !file_ok) {
        open_file->setToolTip(tr("The file has been deleted or moved"));
    }

    QAction* chosen = menu.exec(table_->viewport()->mapToGlobal(pos));
    if (chosen == open_file) {
        open_file_for_row(row);
    } else if (chosen == open_folder) {
        open_folder_for_row(row);
    } else if (chosen == properties) {
        show_properties_for_row(row);
    } else if (chosen == start_item) {
        if (item->queue_id <= 0 || allow_queue_item_start(item->queue_id)) {
            start_task_at_row(row);
        }
    } else if (chosen == pause_item) {
        limit_paused_items_.remove(item->id);
        if (item->task) item->task->pause();
    } else if (chosen == move_to_downloads) {
        move_item_to_downloads(row);
    } else if (move_targets.contains(chosen)) {
        move_item_to_queue(row, move_targets.value(chosen));
    }
}

void MainWindow::refresh_missing_state(int row) {
    DownloadItem* item = model_->item_at(row);
    if (!item || !item->is_complete() || item->dest_path.isEmpty()) return;
    const bool missing = !QFileInfo::exists(item->dest_path);
    if (missing == item->file_missing) return;
    item->file_missing = missing;
    model_->touch_row(row);
}

void MainWindow::show_missing_file_dialog(const DownloadItem& item) {
    auto* box = new QMessageBox(nullptr);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setWindowModality(Qt::NonModal);
    box->setWindowTitle(tr("File Not Found"));
    box->setIconPixmap(dialog_warning_pixmap());
    box->setText(missing_file_warning(item.dest_path));
    const QString dir = QFileInfo(item.dest_path).absolutePath();
    if (!dir.isEmpty() && QFileInfo(dir).isDir()) {
        QPushButton* open_folder =
            box->addButton(tr("Open File Location"), QMessageBox::ActionRole);
        connect(open_folder, &QPushButton::clicked, this,
                [dir] { QDesktopServices::openUrl(QUrl::fromLocalFile(dir)); });
    }
    box->addButton(QMessageBox::Close);
    box->adjustSize();
    box->move(frameGeometry().center() - box->rect().center());
    box->show();
    box->raise();
    box->activateWindow();
}

void MainWindow::open_file_for_row(int row) {
    DownloadItem* item = model_->item_at(row);
    if (!item || item->dest_path.isEmpty()) return;
    if (!QFileInfo::exists(item->dest_path)) {
        // The row may have been Complete when the list was restored but the
        // file is gone now: explain instead of silently doing nothing.
        if (item->is_complete()) {
            item->file_missing = true;
            model_->touch_row(row);
        }
        show_missing_file_dialog(*item);
        return;
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(item->dest_path));
}

void MainWindow::open_folder_for_row(int row) {
    const DownloadItem* item = model_->item_at(row);
    if (!item) return;
    const QString dir =
        item->dest_path.isEmpty() ? item->output_dir : QFileInfo(item->dest_path).absolutePath();
    if (dir.isEmpty()) return;
    if (!QFileInfo(dir).isDir()) {
        auto* box = new QMessageBox(nullptr);
        box->setAttribute(Qt::WA_DeleteOnClose);
        box->setWindowModality(Qt::NonModal);
        box->setWindowTitle(tr("Folder Not Found"));
        box->setIconPixmap(dialog_warning_pixmap());
        box->setText(tr("The folder has been deleted or moved:\n%1").arg(dir));
        box->addButton(QMessageBox::Close);
        box->adjustSize();
        box->move(frameGeometry().center() - box->rect().center());
        box->show();
        return;
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
}

void MainWindow::show_properties_for_row(int row) {
    DownloadItem* item = model_->item_at(row);
    if (!item) return;
    PropertiesDialog dialog(*item, item->task, this);
    dialog.exec();
}

void MainWindow::remove_row_with_cleanup(int row, bool check_queue) {
    DownloadItem* item = model_->item_at(row);
    if (!item) return;
    const int id = item->id;
    const qint64 queue_id = item->queue_id;
    limit_paused_items_.remove(id);
    if (item->history_id > 0 && history_.is_open()) {
        history_.remove(item->history_id);
        last_progress_save_ms_.remove(item->history_id);
    }
    DownloadTask* task = item->task;
    if (task) {
        if (task->active()) {
            // Cleanup + deleteLater happen when finished(Canceled) arrives.
            task->cancel();
        } else {
            if (!task->workspace_dir().isEmpty()) {
                QDir(task->workspace_dir()).removeRecursively();
            }
            task->deleteLater();
        }
    }
    if (QPointer<DownloadWindow> window = windows_.value(id)) window->close();
    windows_.remove(id);
    model_->remove_row(row);

    if (check_queue && queue_id > 0) {
        const swiftdm::HistoryStore::QueueRecord* queue = queue_for(queue_id);
        if (!queue) return;
        if (running_queues_.contains(queue_id)) schedule_queue(queue_id);
        check_queue_completion(queue_id);
        rebuild_queue_tree();
    }
}

void MainWindow::update_action_states() {
    const QList<int> rows = selected_rows();
    const bool overview = queue_mode_ && selected_queue_id_ <= 0;
    const swiftdm::HistoryStore::QueueRecord* queue =
        selected_queue_id_ > 0 ? queue_for(selected_queue_id_) : nullptr;

    bool can_delete_file = false;
    for (const int row : rows) {
        const DownloadItem* item = model_->item_at(row);
        if (item && item->is_complete() && !item->dest_path.isEmpty() &&
            QFileInfo::exists(item->dest_path)) {
            can_delete_file = true;
            break;
        }
    }
    // The table is hidden in the overview, so row actions stay off even if a
    // selection survived the view switch.
    act_delete_->setEnabled(!overview && !rows.isEmpty());
    act_delete_file_->setEnabled(!overview && !rows.isEmpty() && can_delete_file);
    act_new_->setEnabled(!queue_mode_);
    act_queue_add_->setEnabled(queue != nullptr);
    act_new_queue_->setEnabled(queue_features_available());
    act_delete_queue_->setEnabled(queue != nullptr);
    act_queue_props_->setEnabled(queue != nullptr);

    if (queue != nullptr) {
        const std::vector<QueueItemView> views = queue_views(selected_queue_id_);
        act_resume_->setEnabled(false);
        act_stop_->setEnabled(false);
        act_start_queue_->setEnabled(
            !queue_start_order(views, selected_queue_id_, true).empty());
        act_pause_queue_->setEnabled(
            queue_active_count(views, selected_queue_id_) > 0 ||
            running_queues_.contains(selected_queue_id_));
    } else if (queue_mode_) {
        act_resume_->setEnabled(false);
        act_stop_->setEnabled(false);
        act_start_queue_->setEnabled(false);
        act_pause_queue_->setEnabled(false);
    } else {
        ToolbarActionState actions;
        for (const int row : rows) {
            const DownloadItem* item = model_->item_at(row);
            if (!item) continue;
            merge_toolbar_action_state(actions, item->state, item->task != nullptr,
                                       item->task && item->task->active());
        }
        act_resume_->setEnabled(actions.resume);
        act_stop_->setEnabled(actions.stop);
        act_start_queue_->setEnabled(false);
        act_pause_queue_->setEnabled(false);
    }
    update_queue_action_visibility();
}

void MainWindow::on_delete_file() {
    // Only completed downloads whose file is still on disk can be deleted.
    QVector<QPair<int, QString>> targets;  // (item id, file path)
    for (const int row : selected_rows()) {
        const DownloadItem* item = model_->item_at(row);
        if (!item || !item->is_complete() || item->dest_path.isEmpty()) continue;
        if (!QFileInfo::exists(item->dest_path)) continue;
        targets.append({item->id, item->dest_path});
    }
    if (targets.isEmpty()) return;

    QMessageBox box(this);
    box.setWindowTitle(tr("Delete File"));
    box.setIconPixmap(dialog_warning_pixmap());
    if (targets.size() == 1) {
        box.setText(tr("Delete \"%1\" from disk?").arg(QFileInfo(targets.first().second).fileName()));
        box.setInformativeText(
            tr("%1\n\nThis cannot be undone.").arg(targets.first().second));
    } else {
        QStringList names;
        for (const auto& target : targets) names << QFileInfo(target.second).fileName();
        box.setText(tr("Delete %1 files from disk?").arg(targets.size()));
        box.setInformativeText(tr("%1\n\nThis cannot be undone.").arg(names.join('\n')));
    }
    QPushButton* delete_button = box.addButton(tr("Delete"), QMessageBox::DestructiveRole);
    QPushButton* cancel_button = box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(cancel_button);
    box.exec();
    if (box.clickedButton() != delete_button) return;

    QStringList failed;
    for (const auto& target : targets) {
        if (!QFile::remove(target.second)) {
            failed << QFileInfo(target.second).fileName();
            continue;
        }
        // The list entry only refers to the file that is now gone: drop it too.
        const int row = model_->row_for_id(target.first);
        if (row >= 0) remove_row_with_cleanup(row);
    }
    update_status_bar();
    if (!failed.isEmpty()) {
        show_error_dialog(failed.join(QStringLiteral(", ")),
                          tr("Could not delete the file(s) from disk."));
    }
}

void MainWindow::on_resume() {
    const QList<int> rows = selected_rows();
    for (const int row : rows) {
        DownloadItem* item = model_->item_at(row);
        if (!item || !item->task) continue;
        if (!item_can_resume(item->state, true, item->task->active())) continue;
        if (item->queue_id > 0 && !allow_queue_item_start(item->queue_id)) continue;
        const int id = item->id;
        DownloadTask::Options options = item->task->options();
        options.limit_enabled = item->bandwidth_limit_enabled;
        options.limit_kbps = item->bandwidth_limit_kbps;
        start_task_at_row(row, &options);
        open_download_window(id);
    }
}

void MainWindow::on_stop() {
    const QList<int> rows = selected_rows();
    for (const int row : rows) {
        DownloadItem* item = model_->item_at(row);
        if (!item || !item->task) continue;
        if (!item_can_stop(item->state, true, item->task->active())) continue;
        limit_paused_items_.remove(item->id);
        item->task->pause();
    }
}

void MainWindow::on_delete() {
    const QList<int> rows = selected_rows();
    for (const int row : rows) remove_row_with_cleanup(row);
    update_status_bar();
}

void MainWindow::on_settings() {
    SettingsDialog dialog(settings_, this);
    if (dialog.exec() != QDialog::Accepted) return;
    // The appearance settings take effect right away; no restart needed.
    apply_interface_settings();
    apply_tray_visibility();
}

void MainWindow::on_browsers() {
    QString state;
    if (browser_integration_ && browser_integration_->isListening()) {
        state = tr("Listening on 127.0.0.1:%1").arg(browser_integration_->port());
        if (!browser_integration_->lastHandshakeError().isEmpty()) {
            state += tr("\nLast rejected handshake: %1")
                         .arg(browser_integration_->lastHandshakeError());
        }
    } else {
        state = tr("The local browser service is unavailable");
        if (browser_integration_ && !browser_integration_->lastError().isEmpty()) {
            state += QStringLiteral(": %1").arg(browser_integration_->lastError());
        } else {
            state += QLatin1Char('.');
        }
    }
    QMessageBox box(this);
    box.setIconPixmap(dialog_info_pixmap());
    box.setWindowTitle(tr("Browser Extensions"));
    box.setText(tr("Chromium integration (Chrome / Chromium / Edge / Brave)\n%1\n\n"
                   "Firefox based browser integration coming soon.\n\n"
                   "Automatic browser download handoff is enabled in the extension popup.\n")
                    .arg(state));
    box.exec();
}

void MainWindow::on_about() {
    QMessageBox about(this);
    about.setWindowTitle(tr("About SwiftDM"));
    about.setIconPixmap(extension_icon().pixmap(96, 96));
    about.setTextFormat(Qt::RichText);
    about.setText(
        QStringLiteral(
            "<b>SwiftDM</b> %1<br><br>"
            "Simple lightweight download manager with browser integration.<br><br>"
            "Reimplemented from observed behavior of the NDM application "
            "no original code or assets are used.<br><br>"
            "Created By RF Design with AI Agent.")
            .arg(swiftdm::kVersionString));
    about.exec();
}

void MainWindow::on_quit() {
    if (!confirm_quit()) return;
    quitting_ = true;
    QApplication::quit();
}

void MainWindow::on_tree_selection() {
    const QList<QTreeWidgetItem*> items = tree_->selectedItems();
    const QString key =
        items.isEmpty() ? QString() : items.first()->data(0, Qt::UserRole).toString();

    qint64 queue_id = 0;
    if (key.startsWith(QStringLiteral("queue:"))) {
        bool ok = false;
        const qint64 parsed = key.mid(6).toLongLong(&ok);
        if (ok && queue_for(parsed)) queue_id = parsed;
    }
    selected_queue_id_ = queue_id;

    if (key == QStringLiteral("__queues")) {
        queue_mode_ = true;
        selected_queue_id_ = 0;
        if (table_) table_->clearSelection();
        if (right_stack_) right_stack_->setCurrentWidget(queue_overview_);
        proxy_->set_queue_id(-1);
        proxy_->set_category(QString());
        proxy_->set_completion_only(false);
        refresh_queue_overview();
        update_status_bar();
        return;
    }

    if (queue_id > 0) {
        queue_mode_ = true;
        if (right_stack_) right_stack_->setCurrentWidget(table_);
        proxy_->set_queue_id(queue_id);
        update_status_bar();
        return;
    }

    queue_mode_ = false;
    if (right_stack_) right_stack_->setCurrentWidget(table_);
    proxy_->set_queue_id(-1);
    if (key == QStringLiteral("__complete")) {
        proxy_->set_category(QString());
        proxy_->set_completion_only(true);
    } else if (key == QStringLiteral("__incomplete")) {
        proxy_->set_category(QString());
        proxy_->set_incomplete_only(true);
    } else {
        proxy_->set_category(key);
        proxy_->set_completion_only(false);
    }
    update_status_bar();
}

void MainWindow::on_row_activated(const QModelIndex& index) {
    if (!index.isValid()) return;
    const QModelIndex source = proxy_->mapToSource(index);
    DownloadItem* item = model_->item_at(source.row());
    if (!item) return;
    // The file may have disappeared since the row was restored; update the
    // status before the window shows it.
    refresh_missing_state(source.row());
    item = model_->item_at(source.row());
    if (!item) return;
    // Completed rows restored from history have no task, but the window still
    // shows their properties and the missing-file state.
    open_download_window(item->id);
}

void MainWindow::open_download_window(int id) {
    QPointer<DownloadWindow> window = windows_.value(id);
    if (!window) {
        window = new DownloadWindow(model_, id, settings_, this);
        const int row = model_->row_for_id(id);
        if (const DownloadItem* item = model_->item_at(row)) {
            if (item->queue_id > 0) {
                if (const swiftdm::HistoryStore::QueueRecord* queue =
                        queue_for(item->queue_id)) {
                    window->set_queue_context(QString::fromStdString(queue->name),
                                              queue->bandwidth_limit_enabled,
                                              queue->bandwidth_limit_kbps);
                }
            }
        }
        windows_.insert(id, window);
        connect(window, &QObject::destroyed, this, [this, id] { windows_.remove(id); });
        connect(window, &DownloadWindow::resumeRequested, this, [this](int item_id) {
            const int row = model_->row_for_id(item_id);
            if (row < 0) return;
            DownloadItem* item = model_->item_at(row);
            if (!item || !item->task) return;
            if (item->queue_id > 0 && !allow_queue_item_start(item->queue_id)) {
                return;
            }
            DownloadTask::Options options = item->task->options();
            options.limit_enabled = item->bandwidth_limit_enabled;
            options.limit_kbps = item->bandwidth_limit_kbps;
            start_task_at_row(row, &options);
        });
        connect(window, &DownloadWindow::pauseRequested, this, [this](int item_id) {
            const int row = model_->row_for_id(item_id);
            if (row < 0) return;
            DownloadItem* item = model_->item_at(row);
            if (!item || !item->task) return;
            // A manual pause overrides a pause that the queue limit started.
            limit_paused_items_.remove(item->id);
            item->task->pause();
        });
        connect(window, &DownloadWindow::optionsApplied, this, [this](int item_id) {
            const int row = model_->row_for_id(item_id);
            if (row < 0) return;
            DownloadItem* item = model_->item_at(row);
            if (!item || !item->task) return;
            const DownloadTask::Options& options = item->task->options();
            item->output_dir = options.output_dir;
            if (item->queue_id == 0) {
                item->bandwidth_limit_enabled = options.limit_enabled;
                item->bandwidth_limit_kbps = options.limit_kbps;
                item->connections = options.connections;
                item->task->set_live_limit(options.limit_enabled, options.limit_kbps);
            } else {
                // Queue items ignore the per-item editor; only the queue budget
                // applies.
                apply_queue_speed_limits(item->queue_id);
            }
            if (!options.category.isEmpty()) {
                item->category = options.category;
            }
            if (!options.filename.isEmpty()) {
                item->file_name = options.filename;
            }
            if (item->category.isEmpty()) {
                item->category = QString::fromStdString(
                    swiftdm::category_folder_for(item->file_name.toStdString()));
            }
            if (item->history_id > 0 && history_.is_open()) {
                history_.update(item->history_id, history_record_for(*item, &options));
            }
            model_->touch_row(row);
        });
        connect(window, &DownloadWindow::limitEdited, this,
                [this](int item_id, bool enabled, int kbps) {
                    const int row = model_->row_for_id(item_id);
                    if (row < 0) return;
                    DownloadItem* item = model_->item_at(row);
                    if (!item || !item->task) return;
                    if (item->queue_id > 0) {
                        // The per-item editor is disabled for queue items, but
                        // keep the row's value untouched if a stale signal
                        // arrives and reapply the queue split.
                        apply_queue_speed_limits(item->queue_id);
                        return;
                    }
                    item->bandwidth_limit_enabled = enabled;
                    item->bandwidth_limit_kbps = kbps;
                    item->task->set_live_limit(enabled, kbps);
                    if (item->history_id > 0 && history_.is_open()) {
                        history_.update(item->history_id,
                                        history_record_for(*item, &item->task->options()));
                    }
                    model_->touch_row(row);
                });
        connect(window, &QObject::destroyed, this, [this, id] {
            const int row = model_->row_for_id(id);
            if (row < 0) return;
            DownloadItem* item = model_->item_at(row);
            if (!item || !item->task) return;
            if (item->queue_id > 0) {
                apply_queue_speed_limits(item->queue_id);
                return;
            }
            // Keep the last debounced editor state across a pause/resume cycle.
            item->task->set_live_limit(item->bandwidth_limit_enabled,
                                       item->bandwidth_limit_kbps);
        });
    }
    window->show();
    window->raise();
    window->activateWindow();
}

void MainWindow::load_queues() {
    queues_.clear();
    if (!history_.is_open()) return;
    std::vector<swiftdm::HistoryStore::QueueRecord> records;
    std::string error;
    if (!history_.load_queues(&records, &error)) {
        qWarning() << "queues: cannot load:" << QString::fromStdString(error);
        return;
    }
    for (swiftdm::HistoryStore::QueueRecord& record : records) {
        queues_.append(record);
    }
}

void MainWindow::refresh_queue_overview() {
    // rebuild_queue_tree() runs before the overview widgets exist in
    // build_body(), so a null guard is required.
    if (!queue_list_ || !queue_pages_) return;

    // The theme can change while the app runs. Labels with an explicitly set
    // palette do not follow QApplication::setPalette(), so refresh their text
    // color from the current palette whenever the overview is rebuilt.
    const QColor text_color =
        QApplication::palette().color(QPalette::Active, QPalette::WindowText);
    for (QLabel* label : {queue_title_, queue_empty_label_}) {
        if (!label) continue;
        QPalette palette = label->palette();
        palette.setColor(QPalette::WindowText, text_color);
        palette.setColor(QPalette::Disabled, QPalette::WindowText, text_color);
        label->setPalette(palette);
    }

    queue_list_->clear();
    for (const swiftdm::HistoryStore::QueueRecord& queue : queues_) {
        const int count = queue_item_count(queue.id);
        const QString label =
            tr("%1 — %2").arg(QString::fromStdString(queue.name),
                              tr("%n item(s)", nullptr, count));
        auto* item = new QListWidgetItem(
            tinted_icon(QStringLiteral(":/icons/queue.svg"), colors::kActionBlue),
            label, queue_list_);
        item->setData(Qt::UserRole, static_cast<qlonglong>(queue.id));
        item->setToolTip(label);
    }
    const bool has_queues = !queues_.isEmpty();
    queue_pages_->setCurrentIndex(has_queues ? 0 : 1);
    if (queue_empty_label_) {
        queue_empty_label_->setText(
            queue_features_available()
                ? tr("No queues yet.\nCreate a queue to group related downloads.")
                : tr("Queues need the download history database."));
    }
    if (queue_empty_button_) {
        queue_empty_button_->setEnabled(queue_features_available());
        queue_empty_button_->setIcon(
            tinted_icon(QStringLiteral(":/icons/queue-add.svg"), colors::kActionBlue));
    }
}

void MainWindow::rebuild_queue_tree(qint64 select_id) {
    if (!queues_root_) return;
    qint64 target = select_id;
    if (target < 0) target = selected_queue_id_;
    while (queues_root_->childCount() > 0) delete queues_root_->takeChild(0);

    QTreeWidgetItem* found = nullptr;
    for (const swiftdm::HistoryStore::QueueRecord& queue : queues_) {
        auto* child = new QTreeWidgetItem(
            queues_root_, {QString::fromStdString(queue.name)});
        child->setData(0, Qt::UserRole, QStringLiteral("queue:%1").arg(queue.id));
        child->setIcon(
            0, tinted_icon(QStringLiteral(":/icons/queue.svg"), colors::kActionBlue));
        child->setToolTip(
            0, tr("%n item(s)", nullptr, queue_item_count(queue.id)));
        if (queue.id == target) found = child;
    }
    if (queues_.isEmpty()) {
        auto* none = new QTreeWidgetItem(queues_root_, {tr("No queues")});
        none->setFlags(Qt::NoItemFlags);
        none->setData(0, Qt::UserRole, QStringLiteral("__noqueues"));
    }
    queues_root_->setExpanded(true);
    refresh_queue_overview();
    if (target > 0 && found) {
        tree_->setCurrentItem(found);
    } else if (target > 0) {
        // The selected queue disappeared; fall back to the overview instead
        // of the main Downloads list.
        selected_queue_id_ = 0;
        queue_mode_ = true;
        tree_->setCurrentItem(queues_root_);
    } else if (queue_mode_ && selected_queue_id_ <= 0) {
        tree_->setCurrentItem(queues_root_);
    }
}

const swiftdm::HistoryStore::QueueRecord* MainWindow::queue_for(qint64 id) const {
    for (const swiftdm::HistoryStore::QueueRecord& queue : queues_) {
        if (queue.id == id) return &queue;
    }
    return nullptr;
}

swiftdm::HistoryStore::QueueRecord* MainWindow::mutable_queue_for(qint64 id) {
    for (swiftdm::HistoryStore::QueueRecord& queue : queues_) {
        if (queue.id == id) return &queue;
    }
    return nullptr;
}

QVector<QueueChoice> MainWindow::queue_choices() const {
    QVector<QueueChoice> choices;
    choices.reserve(queues_.size());
    for (const swiftdm::HistoryStore::QueueRecord& queue : queues_) {
        choices.append({queue.id, QString::fromStdString(queue.name),
                        queue_item_count(queue.id)});
    }
    return choices;
}

int MainWindow::queue_item_count(qint64 queue_id) const {
    int count = 0;
    for (int row = 0; row < model_->rowCount(); ++row) {
        const DownloadItem* item = model_->item_at(row);
        if (item && item->queue_id == queue_id) ++count;
    }
    return count;
}

int MainWindow::row_for_history(qint64 history_id) const {
    if (history_id <= 0) return -1;
    for (int row = 0; row < model_->rowCount(); ++row) {
        const DownloadItem* item = model_->item_at(row);
        if (item && item->history_id == history_id) return row;
    }
    return -1;
}

std::vector<QueueItemView> MainWindow::queue_views(qint64 queue_id) const {
    std::vector<QueueItemView> views;
    for (int row = 0; row < model_->rowCount(); ++row) {
        const DownloadItem* item = model_->item_at(row);
        if (!item || item->queue_id != queue_id) continue;
        QueueItemView view;
        view.history_id = item->history_id;
        view.queue_id = item->queue_id;
        view.state = item->state;
        view.active = item->task && item->task->active();
        views.push_back(view);
    }
    return views;
}

bool MainWindow::allow_queue_item_start(qint64 queue_id) {
    const swiftdm::HistoryStore::QueueRecord* queue = queue_for(queue_id);
    if (!queue || queue->max_simultaneous <= 0) return true;
    if (queue_active_count(queue_views(queue_id), queue_id) <
        queue->max_simultaneous) {
        return true;
    }
    statusBar()->showMessage(
        tr("Queue \"%1\" is at its simultaneous download limit.")
            .arg(QString::fromStdString(queue->name)),
        6000);
    return false;
}

int MainWindow::queue_share_for_new_task(qint64 queue_id) const {
    const swiftdm::HistoryStore::QueueRecord* queue = queue_for(queue_id);
    if (!queue || !queue->bandwidth_limit_enabled) return 0;
    const int active = queue_active_count(queue_views(queue_id), queue_id) + 1;
    return queue_speed_share_kbps(queue->bandwidth_limit_kbps, active);
}

void MainWindow::apply_queue_speed_limits(qint64 queue_id) {
    const swiftdm::HistoryStore::QueueRecord* queue = queue_for(queue_id);
    if (!queue) return;
    const std::vector<QueueItemView> views = queue_views(queue_id);
    const int active = queue_active_count(views, queue_id);
    const int share = queue->bandwidth_limit_enabled
                          ? queue_speed_share_kbps(queue->bandwidth_limit_kbps, active)
                          : 0;
    for (int row = 0; row < model_->rowCount(); ++row) {
        DownloadItem* item = model_->item_at(row);
        if (!item || item->queue_id != queue_id || !item->task) continue;
        if (!item->task->active()) continue;
        item->task->set_transfer_limit(share);
    }
}

void MainWindow::start_queue(qint64 queue_id) {
    const swiftdm::HistoryStore::QueueRecord* queue = queue_for(queue_id);
    if (!queue) return;
    running_queues_.insert(queue_id);
    const bool unlimited = queue->max_simultaneous <= 0;
    int active = queue_active_count(queue_views(queue_id), queue_id);
    const std::vector<qint64> order =
        queue_start_order(queue_views(queue_id), queue_id, true);
    for (const qint64 history_id : order) {
        if (!unlimited && active >= queue->max_simultaneous) break;
        const int row = row_for_history(history_id);
        DownloadItem* item = model_->item_at(row);
        if (!item || (item->task && item->task->active())) continue;
        start_task_at_row(row);
        ++active;
    }
    apply_queue_speed_limits(queue_id);
    update_status_bar();
}

void MainWindow::pause_queue(qint64 queue_id) {
    running_queues_.remove(queue_id);
    for (int row = 0; row < model_->rowCount(); ++row) {
        DownloadItem* item = model_->item_at(row);
        if (!item || item->queue_id != queue_id || !item->task) continue;
        if (item->task->active()) item->task->pause();
    }
    update_status_bar();
}

void MainWindow::schedule_queue(qint64 queue_id) {
    if (!running_queues_.contains(queue_id)) return;
    const swiftdm::HistoryStore::QueueRecord* queue = queue_for(queue_id);
    if (!queue) {
        running_queues_.remove(queue_id);
        return;
    }
    const bool unlimited = queue->max_simultaneous <= 0;
    int active = queue_active_count(queue_views(queue_id), queue_id);
    if (!unlimited && active >= queue->max_simultaneous) {
        apply_queue_speed_limits(queue_id);
        return;
    }
    const std::vector<qint64> waiting = queue_waiting_order(queue_views(queue_id), queue_id);
    for (const qint64 history_id : waiting) {
        if (!unlimited && active >= queue->max_simultaneous) break;
        const int row = row_for_history(history_id);
        DownloadItem* item = model_->item_at(row);
        if (!item || (item->task && item->task->active())) continue;
        start_task_at_row(row);
        ++active;
    }
    apply_queue_speed_limits(queue_id);
    update_status_bar();
}

void MainWindow::enforce_queue_limit(qint64 queue_id) {
    const swiftdm::HistoryStore::QueueRecord* queue = queue_for(queue_id);
    if (!queue) return;
    if (queue->max_simultaneous > 0) {
        const std::vector<qint64> excess =
            queue_items_to_pause(queue_views(queue_id), queue_id, queue->max_simultaneous);
        for (const qint64 history_id : excess) {
            const int row = row_for_history(history_id);
            DownloadItem* item = model_->item_at(row);
            if (!item || !item->task || !item->task->active()) continue;
            limit_paused_items_.insert(item->id);
            item->task->pause();
        }
    }
    if (running_queues_.contains(queue_id)) schedule_queue(queue_id);
    apply_queue_speed_limits(queue_id);
    update_status_bar();
}

void MainWindow::notify_queue(qint64 queue_id, const QString& title, const QString& text) {
    (void)queue_id;
    if (tray_ && tray_->isVisible() && QSystemTrayIcon::supportsMessages()) {
        tray_->showMessage(title, text, QSystemTrayIcon::Information, 6000);
        return;
    }
    // No tray (or no notification support): the status bar is the fallback.
    statusBar()->showMessage(text, 8000);
}

void MainWindow::check_queue_completion(qint64 queue_id) {
    const swiftdm::HistoryStore::QueueRecord* queue = queue_for(queue_id);
    if (!queue) return;
    const std::vector<QueueItemView> views = queue_views(queue_id);
    int count = 0;
    if (!queue_all_complete(views, queue_id, &count)) {
        complete_notified_.remove(queue_id);
        if (shutdown_scheduler_.pending(queue_id)) cancel_queue_shutdown(queue_id);
        return;
    }
    if (queue->notify_complete && !complete_notified_.contains(queue_id)) {
        complete_notified_.insert(queue_id);
        notify_queue(queue_id, tr("Queue Complete"),
                     tr("All %1 downloads in queue \"%2\" are complete.")
                         .arg(count)
                         .arg(QString::fromStdString(queue->name)));
    }
    if (queue->shutdown_when_complete) {
        // A poweroff is global, so only one countdown is active at a time. If
        // another queue already asked for one, keep that deadline instead of
        // arming a second timer that the dialog cannot show.
        if (shutdown_scheduler_.pending_count() == 0) {
            shutdown_scheduler_.arm(queue_id, queue->shutdown_delay, now_ms());
            show_shutdown_countdown(queue_id);
        }
    }
}

void MainWindow::show_shutdown_countdown(qint64 queue_id) {
    if (shutdown_box_) {
        shutdown_box_->disconnect(this);
        shutdown_box_->close();
        shutdown_box_->deleteLater();
        shutdown_box_ = nullptr;
    }
    shutdown_queue_id_ = queue_id;
    auto* box = new QMessageBox(this);
    box->setObjectName(QStringLiteral("shutdown_countdown"));
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setWindowModality(Qt::NonModal);
    box->setWindowTitle(tr("Shutdown"));
    box->setIconPixmap(dialog_warning_pixmap());
    QPushButton* now = box->addButton(tr("Shut Down Now"), QMessageBox::AcceptRole);
    QPushButton* cancel = box->addButton(QMessageBox::Cancel);
    box->setDefaultButton(cancel);
    connect(now, &QPushButton::clicked, this, [this, queue_id] {
        perform_shutdown(queue_id);
    });
    connect(box, &QDialog::rejected, this, [this, queue_id] {
        cancel_queue_shutdown(queue_id, true);
    });
    connect(box, &QObject::destroyed, this, [this, box] {
        if (shutdown_box_ == box) shutdown_box_ = nullptr;
    });
    shutdown_box_ = box;
    update_shutdown_text();
    box->adjustSize();
    box->move(frameGeometry().center() - box->rect().center());
    box->show();
    box->raise();
    if (shutdown_timer_) shutdown_timer_->start();
}

void MainWindow::update_shutdown_text() {
    if (!shutdown_box_ || shutdown_queue_id_ <= 0) return;
    const swiftdm::HistoryStore::QueueRecord* queue = queue_for(shutdown_queue_id_);
    const int seconds =
        shutdown_scheduler_.remaining_seconds(shutdown_queue_id_, now_ms());
    const QString when = seconds > 0 ? tr("in %1 seconds").arg(seconds) : tr("now");
    shutdown_box_->setText(
        tr("All items in queue \"%1\" are complete.\n\n"
           "Other active downloads and queues will be paused before the computer "
           "shuts down.\n\nShutting down %2.")
            .arg(queue ? QString::fromStdString(queue->name) : QString(), when));
}

void MainWindow::on_shutdown_tick() {
    const qint64 due = shutdown_scheduler_.poll_due(now_ms());
    if (due > 0) {
        perform_shutdown(due);
        return;
    }
    if (shutdown_scheduler_.pending_count() == 0 && shutdown_timer_) {
        shutdown_timer_->stop();
    }
    update_shutdown_text();
}

void MainWindow::cancel_queue_shutdown(qint64 queue_id, bool user_cancelled) {
    shutdown_scheduler_.cancel(queue_id);
    if (shutdown_queue_id_ != queue_id) return;
    shutdown_queue_id_ = 0;
    if (shutdown_timer_) shutdown_timer_->stop();
    if (shutdown_box_) {
        QMessageBox* box = shutdown_box_;
        shutdown_box_ = nullptr;
        box->disconnect(this);
        box->close();
        box->deleteLater();
    }
    if (!user_cancelled) return;
    statusBar()->showMessage(tr("Shutdown cancelled."), 6000);
    const swiftdm::HistoryStore::QueueRecord* queue = queue_for(queue_id);
    if (queue && queue->notify_complete && !complete_notified_.contains(queue_id)) {
        complete_notified_.insert(queue_id);
        notify_queue(queue_id, tr("Queue Complete"),
                     tr("All downloads in queue \"%1\" are complete.")
                         .arg(QString::fromStdString(queue->name)));
    }
}

void MainWindow::perform_shutdown(qint64 queue_id) {
    if (shutting_down_) return;
    if (!queue_all_complete(queue_views(queue_id), queue_id)) {
        cancel_queue_shutdown(queue_id);
        return;
    }
    shutting_down_ = true;
    shutdown_queue_id_ = 0;
    shutdown_scheduler_.cancel_all();
    if (shutdown_timer_) shutdown_timer_->stop();
    if (shutdown_box_) {
        QMessageBox* box = shutdown_box_;
        shutdown_box_ = nullptr;
        box->disconnect(this);
        box->close();
        box->deleteLater();
    }

    // No queue may schedule a new task while the machine is going down.
    running_queues_.clear();
    for (int row = 0; row < model_->rowCount(); ++row) {
        DownloadItem* item = model_->item_at(row);
        if (!item || !item->task) continue;
        if (!item->task->active()) continue;
        item->task->pause_and_wait();
        const DownloadTask::Snapshot snapshot = item->task->snapshot();
        if (!item->is_complete() && snapshot.done > 0) {
            item->done = snapshot.done;
            if (snapshot.total > 0) item->size = snapshot.total;
            item->percent =
                item->size > 0 ? swiftdm::percent_of(item->done, item->size) : 0;
        }
        if (!item->task->workspace_dir().isEmpty()) {
            item->temp_path = item->task->workspace_dir();
        }
        item->state = item->queue_id > 0 ? DownloadItem::State::Queued
                                         : DownloadItem::State::Paused;
        if (item->history_id > 0 && history_.is_open()) {
            history_.update(item->history_id,
                            history_record_for(*item, &item->task->options()));
        }
    }
    update_status_bar();
    statusBar()->showMessage(tr("All downloads paused; shutting down…"), 0);
    qInfo() << "shutdown: queue" << queue_id << "complete; powering off";

    bool started = QProcess::startDetached(QStringLiteral("systemctl"),
                                           {QStringLiteral("poweroff")});
    if (!started) {
        started = QProcess::startDetached(QStringLiteral("shutdown"),
                                          {QStringLiteral("-h"), QStringLiteral("now")});
    }
    if (started) return;

    shutting_down_ = false;
    QMessageBox box(this);
    box.setWindowTitle(tr("Shutdown Failed"));
    box.setIconPixmap(dialog_warning_pixmap());
    box.setText(tr("Could not start the shutdown command.\n\n"
                   "Your active downloads were paused."));
    box.addButton(QMessageBox::Close);
    box.exec();
    update_status_bar();
}

void MainWindow::start_autostart_queues() {
    for (const swiftdm::HistoryStore::QueueRecord& queue : queues_) {
        if (queue.autostart) start_queue(queue.id);
    }
}

void MainWindow::move_item_to_queue(int row, qint64 queue_id) {
    DownloadItem* item = model_->item_at(row);
    if (!item || item->queue_id == queue_id) return;
    if (item->task && item->task->active()) return;
    if (queue_id > 0 && !queue_for(queue_id)) return;

    const qint64 old_queue = item->queue_id;
    item->queue_id = queue_id;
    if (queue_id > 0 && item->state == DownloadItem::State::Paused) {
        item->state = DownloadItem::State::Queued;
    } else if (queue_id == 0 && item->state == DownloadItem::State::Queued) {
        item->state = DownloadItem::State::Paused;
    }
    if (item->history_id > 0 && history_.is_open()) {
        history_.update_queue_id(item->history_id, queue_id, history_status(item->state));
    }
    if (QPointer<DownloadWindow> window = windows_.value(item->id)) {
        if (const swiftdm::HistoryStore::QueueRecord* queue = queue_for(queue_id)) {
            window->set_queue_context(QString::fromStdString(queue->name),
                                      queue->bandwidth_limit_enabled,
                                      queue->bandwidth_limit_kbps);
        } else {
            window->clear_queue_context();
        }
    }
    model_->touch_row(row);
    if (proxy_) proxy_->set_queue_id(selected_queue_id_ > 0 ? selected_queue_id_ : -1);

    if (old_queue > 0) {
        if (running_queues_.contains(old_queue)) schedule_queue(old_queue);
        check_queue_completion(old_queue);
    }
    if (queue_id > 0) {
        // Moving an already-complete row into a queue is not new work: keep a
        // pending all-complete countdown and the one-shot notification state
        // intact. Any other state makes the queue incomplete again.
        if (item->state != DownloadItem::State::Complete) {
            cancel_queue_shutdown(queue_id);
            complete_notified_.remove(queue_id);
            if (running_queues_.contains(queue_id)) schedule_queue(queue_id);
            check_queue_completion(queue_id);
        }
    }
    rebuild_queue_tree();
    update_status_bar();
}

void MainWindow::move_item_to_downloads(int row) {
    move_item_to_queue(row, 0);
}

void MainWindow::move_selected_to_downloads() {
    for (const int row : selected_rows()) move_item_to_downloads(row);
}

void MainWindow::open_queue_properties(qint64 queue_id) {
    swiftdm::HistoryStore::QueueRecord* queue = mutable_queue_for(queue_id);
    if (!queue) return;
    QueuePropertiesDialog dialog(*queue, queue_item_count(queue_id), this);
    if (dialog.exec() != QDialog::Accepted) return;
    const swiftdm::HistoryStore::QueueRecord updated = dialog.result();
    if (history_.is_open()) {
        std::string error;
        if (!history_.update_queue(updated, &error)) {
            show_error_dialog(QString(), QString::fromStdString(error));
            return;
        }
    }
    *queue = updated;
    if (!updated.shutdown_when_complete) cancel_queue_shutdown(queue_id);
    rebuild_queue_tree(queue_id);
    for (int row = 0; row < model_->rowCount(); ++row) {
        const DownloadItem* item = model_->item_at(row);
        if (!item || item->queue_id != queue_id) continue;
        if (QPointer<DownloadWindow> window = windows_.value(item->id)) {
            window->set_queue_context(QString::fromStdString(updated.name),
                                      updated.bandwidth_limit_enabled,
                                      updated.bandwidth_limit_kbps);
        }
    }
    enforce_queue_limit(queue_id);
    update_status_bar();
}

void MainWindow::rename_queue(qint64 queue_id) {
    swiftdm::HistoryStore::QueueRecord* queue = mutable_queue_for(queue_id);
    if (!queue) return;
    bool ok = false;
    const QString name = QInputDialog::getText(
        this, tr("Rename Queue"), tr("Queue name:"), QLineEdit::Normal,
        QString::fromStdString(queue->name), &ok);
    if (!ok) return;
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty()) return;
    swiftdm::HistoryStore::QueueRecord updated = *queue;
    updated.name = trimmed.toStdString();
    if (history_.is_open()) {
        std::string error;
        if (!history_.update_queue(updated, &error)) {
            show_error_dialog(QString(), QString::fromStdString(error));
            return;
        }
    }
    *queue = updated;
    rebuild_queue_tree(queue_id);
    for (int row = 0; row < model_->rowCount(); ++row) {
        const DownloadItem* item = model_->item_at(row);
        if (!item || item->queue_id != queue_id) continue;
        if (QPointer<DownloadWindow> window = windows_.value(item->id)) {
            window->set_queue_context(QString::fromStdString(updated.name),
                                      updated.bandwidth_limit_enabled,
                                      updated.bandwidth_limit_kbps);
        }
    }
    update_status_bar();
}

void MainWindow::delete_queue(qint64 queue_id) {
    const swiftdm::HistoryStore::QueueRecord* queue = queue_for(queue_id);
    if (!queue) return;
    const QString name = QString::fromStdString(queue->name);
    const int count = queue_item_count(queue_id);

    QMessageBox box(this);
    box.setWindowTitle(tr("Delete Queue"));
    box.setIconPixmap(dialog_warning_pixmap());
    box.setText(tr("Delete queue \"%1\" and its %2 download(s)?").arg(name).arg(count));
    box.setInformativeText(
        tr("Partial downloads and temporary files will be removed. Completed files "
           "stay on disk."));
    QPushButton* delete_button = box.addButton(tr("Delete Queue"), QMessageBox::DestructiveRole);
    QPushButton* cancel_button = box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(cancel_button);
    box.exec();
    if (box.clickedButton() != delete_button) return;

    cancel_queue_shutdown(queue_id);
    running_queues_.remove(queue_id);
    complete_notified_.remove(queue_id);
    for (int row = model_->rowCount() - 1; row >= 0; --row) {
        const DownloadItem* item = model_->item_at(row);
        if (item && item->queue_id == queue_id) remove_row_with_cleanup(row, false);
    }
    if (history_.is_open()) {
        std::string error;
        if (!history_.remove_queue(queue_id, &error)) {
            show_error_dialog(QString(), QString::fromStdString(error));
        }
    }
    for (int i = 0; i < queues_.size(); ++i) {
        if (queues_[i].id == queue_id) {
            queues_.removeAt(i);
            break;
        }
    }
    selected_queue_id_ = 0;
    queue_mode_ = true;
    rebuild_queue_tree();
    if (queues_root_) tree_->setCurrentItem(queues_root_);
    proxy_->set_queue_id(-1);
    proxy_->set_category(QString());
    proxy_->set_completion_only(false);
    if (table_) table_->clearSelection();
    if (right_stack_) right_stack_->setCurrentWidget(queue_overview_);
    refresh_queue_overview();
    update_status_bar();
}

void MainWindow::on_new_queue() {
    if (!queue_features_available()) {
        QMessageBox box(this);
        box.setWindowTitle(tr("Queues Unavailable"));
        box.setIconPixmap(dialog_warning_pixmap());
        box.setText(tr("Queues need the download history database, which is not "
                       "available right now."));
        box.addButton(QMessageBox::Close);
        box.exec();
        return;
    }
    const bool from_overview = queue_mode_ && selected_queue_id_ <= 0;
    bool ok = false;
    const QString suggested = tr("Queue %1").arg(queues_.size() + 1);
    const QString name = QInputDialog::getText(this, tr("New Queue"), tr("Queue name:"),
                                               QLineEdit::Normal, suggested, &ok);
    if (!ok) return;
    QString trimmed = name.trimmed();
    if (trimmed.isEmpty()) trimmed = suggested;

    swiftdm::HistoryStore::QueueRecord queue;
    queue.name = trimmed.toStdString();
    queue.max_simultaneous = 1;
    queue.connections = 0;
    queue.bandwidth_limit_enabled = false;
    queue.bandwidth_limit_kbps = 1024;
    queue.shutdown_delay = 60;
    std::string error;
    const int64_t id = history_.insert_queue(queue, &error);
    if (id <= 0) {
        show_error_dialog(QString(), QString::fromStdString(error));
        return;
    }
    queue.id = id;
    queues_.append(queue);
    if (from_overview) {
        // Keep the overview visible so the new queue shows up in the list.
        rebuild_queue_tree();
    } else {
        rebuild_queue_tree(id);
    }
    update_status_bar();
}

void MainWindow::on_delete_queue() {
    if (selected_queue_id_ > 0) delete_queue(selected_queue_id_);
}

void MainWindow::on_queue_overview_clicked(QListWidgetItem* item) {
    if (!item || !queues_root_) return;
    const qint64 queue_id = item->data(Qt::UserRole).toLongLong();
    if (queue_id <= 0) return;
    const QString key = QStringLiteral("queue:%1").arg(queue_id);
    for (int i = 0; i < queues_root_->childCount(); ++i) {
        QTreeWidgetItem* child = queues_root_->child(i);
        if (child && child->data(0, Qt::UserRole).toString() == key) {
            tree_->setCurrentItem(child);
            return;
        }
    }
}

void MainWindow::on_start_queue() {
    if (selected_queue_id_ > 0) start_queue(selected_queue_id_);
}

void MainWindow::on_pause_queue() {
    if (selected_queue_id_ > 0) pause_queue(selected_queue_id_);
}

void MainWindow::on_queue_properties() {
    if (selected_queue_id_ > 0) open_queue_properties(selected_queue_id_);
}

void MainWindow::on_tree_context_menu(const QPoint& pos) {
    QTreeWidgetItem* tree_item = tree_->itemAt(pos);
    if (!tree_item) return;
    qint64 queue_id = 0;
    const QString key = tree_item->data(0, Qt::UserRole).toString();
    if (key.startsWith(QStringLiteral("queue:"))) {
        bool ok = false;
        const qint64 parsed = key.mid(6).toLongLong(&ok);
        if (ok) queue_id = parsed;
    }
    if (tree_item != queues_root_ && queue_id <= 0) return;

    QMenu menu(this);
    QAction* new_queue = menu.addAction(
        tinted_icon(QStringLiteral(":/icons/queue-add.svg"), colors::kActionBlue),
        tr("New Queue"));
    QAction* start = nullptr;
    QAction* pause = nullptr;
    QAction* add_url = nullptr;
    QAction* rename = nullptr;
    QAction* properties = nullptr;
    QAction* delete_action = nullptr;
    if (queue_id > 0) {
        menu.addSeparator();
        start = menu.addAction(
            tinted_icon(QStringLiteral(":/icons/resume-start.svg"), colors::kActionBlue),
            tr("Start Queue"));
        pause = menu.addAction(
            tinted_icon(QStringLiteral(":/icons/stop.svg"), colors::kActionBlue),
            tr("Stop Queue"));
        add_url = menu.addAction(
            tinted_icon(QStringLiteral(":/icons/link-plus.svg"), colors::kActionBlue),
            tr("New URL"));
        menu.addSeparator();
        rename = menu.addAction(tr("Rename"));
        properties = menu.addAction(
            tinted_icon(QStringLiteral(":/icons/queue-settings.svg"), colors::kNeutral),
            tr("Properties"));
        delete_action = menu.addAction(
            tinted_icon(QStringLiteral(":/icons/queue-delete.svg"), colors::kDanger),
            tr("Delete Queue"));
    }

    QAction* chosen = menu.exec(tree_->viewport()->mapToGlobal(pos));
    if (!chosen) return;
    if (chosen == new_queue) {
        on_new_queue();
    } else if (chosen == start) {
        start_queue(queue_id);
    } else if (chosen == pause) {
        pause_queue(queue_id);
    } else if (chosen == add_url) {
        tree_->setCurrentItem(tree_item);
        on_new_url();
    } else if (chosen == rename) {
        rename_queue(queue_id);
    } else if (chosen == properties) {
        open_queue_properties(queue_id);
    } else if (chosen == delete_action) {
        delete_queue(queue_id);
    }
}

void MainWindow::update_queue_action_visibility() {
    if (!act_start_queue_) return;
    const bool queue_mode = queue_mode_;
    act_new_->setVisible(!queue_mode);
    act_resume_->setVisible(!queue_mode);
    act_stop_->setVisible(!queue_mode);
    act_queue_add_->setVisible(queue_mode);
    act_start_queue_->setVisible(queue_mode);
    act_pause_queue_->setVisible(queue_mode);
    act_queue_props_->setVisible(queue_mode);
    act_new_queue_->setVisible(queue_mode);
    act_delete_queue_->setVisible(queue_mode);
    if (sep_after_new_) sep_after_new_->setVisible(!queue_mode);
    if (sep_download_actions_) sep_download_actions_->setVisible(!queue_mode);
    if (sep_queue_actions_) sep_queue_actions_->setVisible(queue_mode);
    if (sep_queue_transfer_actions_) sep_queue_transfer_actions_->setVisible(queue_mode);
}

void MainWindow::update_status_bar() {
    int active = 0;
    int queued = 0;
    int paused = 0;
    int complete = 0;
    int failed = 0;
    int scoped_total = 0;
    int global_active = 0;
    for (int row = 0; row < model_->rowCount(); ++row) {
        const DownloadItem* item = model_->item_at(row);
        if (!item) continue;
        const bool is_active = item->state == DownloadItem::State::Downloading ||
                               item->state == DownloadItem::State::Probing;
        if (is_active) ++global_active;
        const bool in_scope = selected_queue_id_ > 0
                                  ? item->queue_id == selected_queue_id_
                                  : (!queue_mode_ && item->queue_id == 0);
        if (!in_scope) continue;
        ++scoped_total;
        switch (item->state) {
            case DownloadItem::State::Downloading:
            case DownloadItem::State::Probing:
                ++active;
                break;
            case DownloadItem::State::Queued:
                ++queued;
                break;
            case DownloadItem::State::Paused:
                ++paused;
                break;
            case DownloadItem::State::Complete:
                ++complete;
                break;
            case DownloadItem::State::Error:
                ++failed;
                break;
        }
    }
    if (queue_mode_ && selected_queue_id_ <= 0) {
        int queue_items = 0;
        for (const swiftdm::HistoryStore::QueueRecord& queue : queues_) {
            queue_items += queue_item_count(queue.id);
        }
        status_label_->setText(
            tr("Queues — %1 — %2")
                .arg(tr("%n queue(s)", nullptr, static_cast<int>(queues_.size())),
                     tr("%n item(s)", nullptr, queue_items)));
    } else if (selected_queue_id_ > 0) {
        const swiftdm::HistoryStore::QueueRecord* queue = queue_for(selected_queue_id_);
        status_label_->setText(
            tr("Queue \"%1\" — %2 items — %3 active, %4 queued, %5 paused, %6 complete, "
               "%7 error")
                .arg(queue ? QString::fromStdString(queue->name) : QString())
                .arg(scoped_total)
                .arg(active)
                .arg(queued)
                .arg(paused)
                .arg(complete)
                .arg(failed));
    } else {
        status_label_->setText(
            tr("%1 downloads — %2 active, %3 queued, %4 paused, %5 complete, %6 error")
                .arg(scoped_total)
                .arg(active)
                .arg(queued)
                .arg(paused)
                .arg(complete)
                .arg(failed));
    }
    // Push the app status to subscribed extension sockets (idle/busy), which is
    // the local equivalent of the original application's waiting/nowaiting
    // notifications.
    if (browser_integration_) browser_integration_->set_status(global_active == 0, global_active);
    update_action_states();
}

}  // namespace swiftdm::ui
