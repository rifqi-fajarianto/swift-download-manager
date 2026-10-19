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
#include <QFontMetrics>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QJsonArray>
#include <QLabel>
#include <QJsonObject>
#include <QMenu>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QSplitter>
#include <QStatusBar>
#include <QSystemTrayIcon>
#include <QTableView>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QUrl>

#include <algorithm>
#include <functional>

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
#include "ui/settings_dialog.h"

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
// and forwarded headers come from the task options when a task exists.
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
        record.connections = options->connections;
        record.bandwidth_limit_kbps = options->limit_kbps;
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
DownloadItem::State state_from_record(const swiftdm::HistoryStore::Record& record) {
    if (record.status == "Complete") return DownloadItem::State::Complete;
    if (record.status == "Paused") return DownloadItem::State::Paused;
    if (record.status == "Error") return DownloadItem::State::Error;
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

    model_ = new DownloadModel(this);
    proxy_ = new DownloadFilterProxy(this);
    proxy_->setSourceModel(model_);

    build_actions();
    build_body();
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

    connect(model_, &QAbstractItemModel::rowsInserted, this,
            [this] { update_status_bar(); });
    connect(model_, &QAbstractItemModel::rowsRemoved, this,
            [this] { update_status_bar(); });
    connect(model_, &QAbstractItemModel::dataChanged, this,
            [this] { update_status_bar(); });
    restore_history();
    update_status_bar();
}

MainWindow::~MainWindow() {
    // Pause every running transfer and wait for its worker so segments.json is
    // written before the process goes away; the list then restores as Paused.
    for (int row = 0; row < model_->rowCount(); ++row) {
        DownloadItem* item = model_->item_at(row);
        if (!item || !item->task) continue;
        const bool was_active = item->task->active();
        item->task->pause_and_wait();
        if (was_active && (item->state == DownloadItem::State::Downloading ||
                           item->state == DownloadItem::State::Probing)) {
            item->state = DownloadItem::State::Paused;
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

    QStringList missing_files;
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
        item.resumable = record.resumable;
        item.temp_path = QString::fromStdString(record.temp_path);

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
                missing_files << item.dest_path;
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
            if (item.state == DownloadItem::State::Paused && record.status != "Paused") {
                // A row left "Downloading"/"Queued" by a quit or crash. Write the
                // full row so lasttry (sort order) stays unchanged.
                record.status = "Paused";
                record.done = item.done;
                record.file_size = item.size;
                history_.update(entry.id, record);
            }
        }
        model_->add_item(item);
    }
    if (!missing_files.isEmpty()) {
        // One summary instead of a dialog per file.
        QTimer::singleShot(0, this, [this, missing_files] {
            show_missing_files_dialog(missing_files);
        });
    }
    qInfo() << "history: restored" << model_->rowCount() << "row(s),"
            << missing_files.size() << "missing file(s)";
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
        new QAction(tinted_icon(QStringLiteral(":/icons/settings.svg"), colors::kNeutral),
                    tr("Settings"), this);
    act_browsers_ =
        new QAction(tinted_icon(QStringLiteral(":/icons/browsers.svg"), colors::kNeutral),
                    tr("Extensions"), this);
    act_about_ = new QAction(tinted_icon(QStringLiteral(":/icons/about.svg"), colors::kActionBlue),
                             tr("About"), this);
    act_quit_ = new QAction(tinted_icon(QStringLiteral(":/icons/quit.svg"), colors::kDanger),
                            tr("Quit"), this);

    act_new_->setShortcut(QKeySequence::New);
    act_delete_->setShortcut(QKeySequence::Delete);

    connect(act_new_, &QAction::triggered, this, &MainWindow::on_new_url);
    connect(act_resume_, &QAction::triggered, this, &MainWindow::on_resume);
    connect(act_stop_, &QAction::triggered, this, &MainWindow::on_stop);
    connect(act_delete_, &QAction::triggered, this, &MainWindow::on_delete);
    connect(act_delete_file_, &QAction::triggered, this, &MainWindow::on_delete_file);
    connect(act_settings_, &QAction::triggered, this, &MainWindow::on_settings);
    connect(act_browsers_, &QAction::triggered, this, &MainWindow::on_browsers);
    connect(act_about_, &QAction::triggered, this, &MainWindow::on_about);
    connect(act_quit_, &QAction::triggered, this, &MainWindow::on_quit);

    auto* toolbar = addToolBar(tr("Main"));
    toolbar->setObjectName(QStringLiteral("main_toolbar"));
    toolbar->setMovable(false);
    toolbar->setIconSize(QSize(28, 28));
    toolbar->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    toolbar->addAction(act_new_);
    toolbar->addAction(act_resume_);
    toolbar->addAction(act_stop_);
    toolbar->addAction(act_delete_);
    toolbar->addAction(act_delete_file_);
    auto* spacer = new QWidget(toolbar);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    toolbar->addWidget(spacer);
    toolbar->addAction(act_settings_);
    toolbar->addAction(act_browsers_);
    toolbar->addAction(act_about_);
    toolbar->addAction(act_quit_);
    for (QAction* action : toolbar->actions()) {
        if (auto* button = qobject_cast<QToolButton*>(toolbar->widgetForAction(action))) {
            button->setFixedSize(88, 76);
        }
    }
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
    tree_->setCurrentItem(all);
    connect(tree_, &QTreeWidget::itemSelectionChanged, this, &MainWindow::on_tree_selection);

    table_ = new QTableView(splitter);
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

    splitter->addWidget(tree_);
    splitter->addWidget(table_);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    setCentralWidget(splitter);

    status_label_ = new QLabel(this);
    statusBar()->addWidget(status_label_);
}

void MainWindow::build_tray() {
    if (!QSystemTrayIcon::isSystemTrayAvailable()) return;

    tray_ = new QSystemTrayIcon(tray_icon(), this);
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

void MainWindow::closeEvent(QCloseEvent* event) {
    if (!quitting_ && tray_) {
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
    event->accept();
    // quitOnLastWindowClosed is disabled for the tray application, so close the
    // process explicitly when there is no tray icon to hide into.
    QApplication::quit();
}

void MainWindow::on_new_url() {
    AddDownloadDialog dialog(this);
    if (dialog.exec() != QDialog::Accepted) return;
    const QString url = dialog.url();
    if (url.isEmpty()) return;
    add_download(url);
}

void MainWindow::add_download(const QString& url) {
    DownloadTask::Options options;
    options.url = url;
    options.output_dir = QString::fromStdString(settings_.download_dir);

    DownloadItem item;
    item.url = url;
    item.file_name = QString::fromStdString(swiftdm::file_name_from_url(url.toStdString()));
    item.category =
        QString::fromStdString(swiftdm::category_folder_for(item.file_name.toStdString()));
    item.output_dir = options.output_dir;
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
    options.url = url.toString();
    options.output_dir = QString::fromStdString(settings_.download_dir);
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

    auto* probe = new DownloadTask(settings_, probe_options, this);
    connect(probe, &DownloadTask::probe_completed, this,
            [this, probe, options, item](bool ok, const QString& error) {
                const QString probed_name = probe->resolved_filename();
                const quint64 probed_size = probe->resolved_size();
                const bool resumable = probe->resumable();
                probe->deleteLater();
                if (!ok) {
                    if (!error.isEmpty()) show_error_dialog(options.url, error, item.page_url);
                    return;
                }

                QString name = QString::fromStdString(
                    swiftdm::sanitize_filename(probed_name.toStdString()));
                if (name.isEmpty()) {
                    name = QString::fromStdString(
                        swiftdm::sanitize_filename(item.file_name.toStdString()));
                }
                if (name.isEmpty()) name = QStringLiteral("download.bin");

                DownloadItem draft = item;
                draft.url = options.url;
                draft.file_name = name;
                draft.category = QString::fromStdString(
                    swiftdm::category_folder_for(name.toStdString()));
                draft.size = probed_size;
                draft.resumable = resumable;
                draft.output_dir = QString::fromStdString(settings_.download_dir);
                draft.last_try = now_stamp();
                draft.last_try_epoch = swiftdm::unix_now();
                draft.state = DownloadItem::State::Queued;

                // A browser handoff can arrive while the app sits in the
                // notification area; the confirmation must still be visible.
                if (isHidden()) {
                    showNormal();
                    raise();
                    activateWindow();
                }
                ConfirmDownloadDialog dialog(name, probed_size, options.url,
                                             QString::fromStdString(settings_.download_dir),
                                             settings_.category_folders, this);
                if (dialog.exec() != QDialog::Accepted) {
                    // Cancel: no list row and no database entry was created.
                    return;
                }

                draft.category = dialog.category();
                draft.output_dir = dialog.location();
                DownloadTask::Options start_options = options;
                start_options.filename = name;
                start_options.category = draft.category;
                start_options.apply_category_folder = false;
                start_options.output_dir = draft.output_dir;
                start_confirmed_download(std::move(draft), std::move(start_options));
            });
    probe->start_probe();
}

void MainWindow::start_confirmed_download(DownloadItem item, DownloadTask::Options options) {
    const int row = model_->add_item(item);
    DownloadItem* added = model_->item_at(row);
    if (!added) return;
    if (history_.is_open()) {
        added->history_id = history_.insert(history_record_for(*added, &options));
    }
    const int id = added->id;
    start_task_at_row(row, &options);
    open_download_window(id);
}

void MainWindow::start_task_at_row(int row, const DownloadTask::Options* options_override) {
    DownloadItem* item = model_->item_at(row);
    if (!item) return;

    // Replace the previous (inactive) task so that current settings apply.
    if (item->task) {
        item->task->deleteLater();
        item->task = nullptr;
    }

    DownloadTask::Options options;
    if (options_override) {
        options = *options_override;
    } else {
        options.url = item->url;
        options.output_dir = item->output_dir;
        options.category = item->category;
        options.connections = 0;   // 0 = settings
        options.limit_kbps = 0;    // 0 = settings
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
    item->task = task;
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
    model_->task_finished(task, status, path, error);
    const int row = model_->row_for_task(task);
    if (row >= 0) {
        DownloadItem* item = model_->item_at(row);
        if (item) {
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
        } else if (status == static_cast<int>(DownloadTask::Status::Failed) && item) {
            show_error_dialog(item->url, error, item->page_url);
        }
    } else {
        // The row was removed while the task was running; clean up leftovers.
        if (!task->workspace_dir().isEmpty()) {
            QDir(task->workspace_dir()).removeRecursively();
        }
        task->deleteLater();
    }
    update_status_bar();
}

void MainWindow::show_completion_dialog(const QString& name, const QString& path) {
    // Independent top-level window: no parent/transient hint, so the window
    // manager cannot group it with the main window when it is moved. Non-modal,
    // so the main window stays usable while the notification is on screen.
    auto* box = new QMessageBox(nullptr);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setWindowModality(Qt::NonModal);
    box->setWindowTitle(tr("Download Complete"));
    box->setIcon(QMessageBox::Information);
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
    box->setIcon(QMessageBox::Critical);
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

void MainWindow::show_missing_files_dialog(const QStringList& paths) {
    // One summary for the whole restore instead of a dialog per file.
    auto* box = new QMessageBox(nullptr);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setWindowModality(Qt::NonModal);
    box->setWindowTitle(tr("Downloaded Files Missing"));
    box->setIcon(QMessageBox::Warning);
    box->setText(tr("Some completed downloads are no longer on disk. "
                    "They were deleted or moved:"));
    QStringList shown = paths.mid(0, 10);
    QString details = shown.join(QLatin1Char('\n'));
    if (paths.size() > shown.size()) {
        details += tr("\n... and %1 more").arg(paths.size() - shown.size());
    }
    box->setInformativeText(details);
    box->addButton(QMessageBox::Close);
    box->adjustSize();
    box->move(frameGeometry().center() - box->rect().center());
    box->show();
    box->raise();
    box->activateWindow();
}

void MainWindow::show_missing_file_dialog(const DownloadItem& item) {
    auto* box = new QMessageBox(nullptr);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setWindowModality(Qt::NonModal);
    box->setWindowTitle(tr("File Not Found"));
    box->setIcon(QMessageBox::Warning);
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
        box->setIcon(QMessageBox::Warning);
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

void MainWindow::remove_row_with_cleanup(int row) {
    DownloadItem* item = model_->item_at(row);
    if (!item) return;
    const int id = item->id;
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
}

void MainWindow::update_action_states() {
    const bool has_selection = !selected_rows().isEmpty();
    act_resume_->setEnabled(has_selection);
    act_stop_->setEnabled(has_selection);
    act_delete_->setEnabled(has_selection);

    bool can_delete_file = false;
    for (const int row : selected_rows()) {
        const DownloadItem* item = model_->item_at(row);
        if (item && item->is_complete() && !item->dest_path.isEmpty() &&
            QFileInfo::exists(item->dest_path)) {
            can_delete_file = true;
            break;
        }
    }
    act_delete_file_->setEnabled(has_selection && can_delete_file);
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
    box.setIcon(QMessageBox::Warning);
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
        if (!item || !item->task || item->task->active()) continue;
        const DownloadItem::State state = item->state;
        if (state != DownloadItem::State::Paused && state != DownloadItem::State::Error &&
            state != DownloadItem::State::Queued) {
            continue;
        }
        const int id = item->id;
        const DownloadTask::Options options = item->task->options();
        start_task_at_row(row, &options);
        open_download_window(id);
    }
}

void MainWindow::on_stop() {
    const QList<int> rows = selected_rows();
    for (const int row : rows) {
        DownloadItem* item = model_->item_at(row);
        if (item && item->state == DownloadItem::State::Downloading && item->task &&
            item->task->active()) {
            item->task->pause();
        }
    }
}

void MainWindow::on_delete() {
    const QList<int> rows = selected_rows();
    for (const int row : rows) remove_row_with_cleanup(row);
    update_status_bar();
}

void MainWindow::on_settings() {
    SettingsDialog dialog(settings_, this);
    dialog.exec();
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
    QMessageBox::information(
        this, tr("Browser Extensions"),
        tr("Chromium integration (Chrome / Chromium / Edge / Brave)\n%1\n\n"
            "Firefox based browser integration coming soon.\n\n"
           "Automatic browser download handoff is enabled in the extension popup.\n")
            .arg(state));
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
    quitting_ = true;
    QApplication::quit();
}

void MainWindow::on_tree_selection() {
    const QList<QTreeWidgetItem*> items = tree_->selectedItems();
    const QString key =
        items.isEmpty() ? QString() : items.first()->data(0, Qt::UserRole).toString();
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
        windows_.insert(id, window);
        connect(window, &QObject::destroyed, this, [this, id] { windows_.remove(id); });
        connect(window, &DownloadWindow::resumeRequested, this, [this](int item_id) {
            const int row = model_->row_for_id(item_id);
            if (row < 0) return;
            DownloadItem* item = model_->item_at(row);
            if (!item || !item->task) return;
            const DownloadTask::Options options = item->task->options();
            start_task_at_row(row, &options);
        });
        connect(window, &DownloadWindow::optionsApplied, this, [this](int item_id) {
            const int row = model_->row_for_id(item_id);
            if (row < 0) return;
            DownloadItem* item = model_->item_at(row);
            if (!item || !item->task) return;
            const DownloadTask::Options& options = item->task->options();
            item->output_dir = options.output_dir;
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
    }
    window->show();
    window->raise();
    window->activateWindow();
}

void MainWindow::update_status_bar() {
    int active = 0;
    int paused = 0;
    int complete = 0;
    int failed = 0;
    for (int row = 0; row < model_->rowCount(); ++row) {
        const DownloadItem* item = model_->item_at(row);
        if (!item) continue;
        switch (item->state) {
            case DownloadItem::State::Downloading:
            case DownloadItem::State::Queued:
            case DownloadItem::State::Probing:
                ++active;
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
    status_label_->setText(
        tr("%1 downloads — %2 active, %3 paused, %4 complete, %5 error")
            .arg(model_->rowCount())
            .arg(active)
            .arg(paused)
            .arg(complete)
            .arg(failed));
    // Push the app status to subscribed extension sockets (idle/busy), which is
    // the local equivalent of the original application's waiting/nowaiting
    // notifications.
    if (browser_integration_) browser_integration_->set_status(active == 0, active);
    update_action_states();
}

}  // namespace swiftdm::ui
