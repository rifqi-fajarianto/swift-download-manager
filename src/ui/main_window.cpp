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
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QJsonArray>
#include <QLabel>
#include <QJsonObject>
#include <QMenu>
#include <QMessageBox>
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

}  // namespace

MainWindow::MainWindow(swiftdm::Settings settings, QWidget* parent)
    : QMainWindow(parent), settings_(std::move(settings)) {
    setWindowTitle(QStringLiteral("SwiftDM %1").arg(swiftdm::kVersionString));
    resize(1020, 580);

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
    update_status_bar();
}

MainWindow::~MainWindow() = default;

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

    tray_ = new QSystemTrayIcon(application_icon(), this);
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
    DownloadItem item;
    item.url = url;
    item.file_name = QString::fromStdString(swiftdm::file_name_from_url(url.toStdString()));
    item.category =
        QString::fromStdString(swiftdm::category_folder_for(item.file_name.toStdString()));
    item.output_dir = QString::fromStdString(settings_.download_dir);
    item.last_try = now_stamp();
    item.state = DownloadItem::State::Probing;
    const int row = model_->add_item(item);
    start_probe_at_row(row);
}

void MainWindow::add_browser_download(const QJsonObject& data) {
    const QUrl url(data.value(QStringLiteral("url")).toString());
    if (!url.isValid() || (url.scheme() != QStringLiteral("http") &&
                           url.scheme() != QStringLiteral("https")) ||
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

    qInfo() << "browser integration: adding download" << options.url;
    DownloadItem item;
    item.url = options.url;
    item.file_name = options.filename.isEmpty()
                         ? QString::fromStdString(
                               swiftdm::file_name_from_url(options.url.toStdString()))
                         : options.filename;
    item.category = QString::fromStdString(
        swiftdm::category_folder_for(item.file_name.toStdString()));
    item.output_dir = options.output_dir;
    item.last_try = now_stamp();
    item.state = DownloadItem::State::Queued;
    const int row = model_->add_item(item);
    const int id = model_->item_at(row)->id;
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
        options.connections = 0;   // 0 = settings
        options.limit_kbps = 0;    // 0 = settings
    }

    auto* task = new DownloadTask(settings_, options, model_);
    item->task = task;
    item->state = DownloadItem::State::Downloading;
    item->last_try = now_stamp();

    connect(task, &DownloadTask::probed, this, [this, task] {
        model_->task_probed(task);
        update_status_bar();
    });
    connect(task, &DownloadTask::progressed, this,
            [this, task] { model_->task_progressed(task); });
    connect(task, &DownloadTask::finished, this,
            [this, task](int status, const QString& path, const QString& error) {
                on_task_finished(task, status, path, error);
            });

    model_->touch_row(row);
    task->start();
    update_status_bar();
}

void MainWindow::start_probe_at_row(int row) {
    DownloadItem* item = model_->item_at(row);
    if (!item) return;

    if (item->task) {
        item->task->deleteLater();
        item->task = nullptr;
    }

    DownloadTask::Options options;
    options.url = item->url;
    options.output_dir = item->output_dir;

    auto* task = new DownloadTask(settings_, options, model_);
    item->task = task;
    item->state = DownloadItem::State::Probing;
    item->last_try = now_stamp();

    connect(task, &DownloadTask::probed, this, [this, task] {
        model_->task_probed(task);
        update_status_bar();
    });
    connect(task, &DownloadTask::probe_completed, this,
            [this, task](bool ok, const QString& error) { on_probe_completed(task, ok, error); });

    model_->touch_row(row);
    task->start_probe();
    update_status_bar();
}

void MainWindow::on_probe_completed(DownloadTask* task, bool ok, const QString& error) {
    const int row = model_->row_for_task(task);
    if (row < 0) {
        // The row was removed while the probe was still running.
        task->deleteLater();
        return;
    }
    DownloadItem* item = model_->item_at(row);
    if (!ok) {
        if (error.isEmpty()) return;  // canceled probe; the row is already handled
        item->state = DownloadItem::State::Error;
        item->error = error;
        model_->touch_row(row);
        show_error_dialog(item->url, error);
        update_status_bar();
        return;
    }

    // Confirmation step: show the resolved file name and size first.
    const QString name = item->file_name;
    const quint64 size = item->size;
    const QString url = item->url;
    const int id = item->id;
    const DownloadTask::Options options = task->options();

    ConfirmDownloadDialog dialog(name, size, url, this);
    if (dialog.exec() != QDialog::Accepted) {
        if (QPointer<DownloadWindow> window = windows_.value(id)) window->close();
        windows_.remove(id);
        model_->remove_row(row);
        task->deleteLater();
        update_status_bar();
        return;
    }
    start_task_at_row(row, &options);
    open_download_window(id);
}

void MainWindow::on_task_finished(DownloadTask* task, int status, const QString& path,
                                  const QString& error) {
    model_->task_finished(task, status, path, error);
    const int row = model_->row_for_task(task);
    if (row >= 0) {
        const DownloadItem* item = model_->item_at(row);
        if (status == static_cast<int>(DownloadTask::Status::Completed)) {
            if (settings_.completion_dialog && item) {
                show_completion_dialog(item->file_name, path);
            }
        } else if (status == static_cast<int>(DownloadTask::Status::Failed) && item) {
            show_error_dialog(item->url, error);
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
    connect(box, &QMessageBox::buttonClicked, box,
            [box, open_folder, open_file, path](QAbstractButton* button) {
                if (button == open_folder) {
                    QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
                } else if (button == open_file) {
                    QDesktopServices::openUrl(QUrl::fromLocalFile(path));
                }
                // Close after the button signal has finished; closing inside the
                // handler can race QMessageBox's own close/delete handling.
                QTimer::singleShot(0, box, &QWidget::close);
            });
    box->adjustSize();
    box->move(frameGeometry().center() - box->rect().center());
    box->show();
    box->raise();
    box->activateWindow();
}

void MainWindow::show_error_dialog(const QString& url, const QString& error) {
    // Same rationale as the completion window: an independent top-level window.
    auto* box = new QMessageBox(nullptr);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setWindowModality(Qt::NonModal);
    box->setWindowTitle(tr("Download Error"));
    box->setIcon(QMessageBox::Critical);
    box->setText(tr("Error : %1").arg(error));
    box->setInformativeText(url);
    box->addButton(QMessageBox::Close);
    connect(box, &QMessageBox::buttonClicked, box,
            [box](QAbstractButton*) { QTimer::singleShot(0, box, &QWidget::close); });
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

    const QFileInfo file(item->dest_path);
    const bool file_ok = item->is_complete() && !item->dest_path.isEmpty() && file.exists();
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
    open_file->setEnabled(file_ok);
    open_folder->setEnabled(dir_ok);

    QAction* chosen = menu.exec(table_->viewport()->mapToGlobal(pos));
    if (chosen == open_file) {
        open_file_for_row(row);
    } else if (chosen == open_folder) {
        open_folder_for_row(row);
    } else if (chosen == properties) {
        show_properties_for_row(row);
    }
}

void MainWindow::open_file_for_row(int row) {
    const DownloadItem* item = model_->item_at(row);
    if (!item || item->dest_path.isEmpty()) return;
    if (!QFileInfo::exists(item->dest_path)) {
        show_error_dialog(item->url, tr("File not found:\n%1").arg(item->dest_path));
        return;
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(item->dest_path));
}

void MainWindow::open_folder_for_row(int row) {
    const DownloadItem* item = model_->item_at(row);
    if (!item) return;
    const QString dir =
        item->dest_path.isEmpty() ? item->output_dir : QFileInfo(item->dest_path).absolutePath();
    if (dir.isEmpty() || !QFileInfo(dir).isDir()) return;
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
           "Automatic browser download handoff is enabled in the extension popup.\n"
           "Load the unpacked development extension from ext/chromium, and reload it\n"
           "after updating the extension files.\n\n"
           "If the popup cannot connect, check the status above. Port 10007 must be\n"
           "free: a hidden SwiftDM tray instance or another program may be using it.\n"
           "Quit the old instance from the tray, then restart SwiftDM.\n\n"
           "Setup and limitations are documented in ext/chromium/README.md.")
            .arg(state));
}

void MainWindow::on_about() {
    QMessageBox::about(
        this, tr("About SwiftDM"),
        QStringLiteral(
            "<b>SwiftDM</b> %1<br><br>"
            "Clean-room Linux download manager (segmented, resumable).<br>"
            "Reimplemented from observed behavior of the Windows application; "
            "no original code or assets are used.<br><br>"
            "No update check, no phone-home.")
            .arg(swiftdm::kVersionString));
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
    if (!item || !item->task) return;
    open_download_window(item->id);
}

void MainWindow::open_download_window(int id) {
    QPointer<DownloadWindow> window = windows_.value(id);
    if (!window) {
        window = new DownloadWindow(model_, id, settings_, this);
        windows_.insert(id, window);
        connect(window, &QObject::destroyed, this, [this, id] { windows_.remove(id); });
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
    update_action_states();
}

}  // namespace swiftdm::ui
