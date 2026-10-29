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
#include <QMoveEvent>
#include <QProcess>
#include <QPushButton>
#include <QResizeEvent>
#include <QScreen>
#include <QShowEvent>
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

namespace {
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

// A saved window rectangle is only restored when enough of it still overlaps a
// screen; otherwise a removed monitor (or a smaller resolution) would put the
// window out of reach.
constexpr int kWindowVisibleWidth = 140;
constexpr int kWindowVisibleHeight = 40;

bool window_rect_reachable(const QRect& rect) {
    for (const QScreen* screen : QGuiApplication::screens()) {
        const QRect visible = rect.intersected(screen->availableGeometry());
        if (visible.width() >= kWindowVisibleWidth &&
            visible.height() >= kWindowVisibleHeight) {
            return true;
        }
    }
    return false;
}

// Picks the screen with the largest overlap with the saved rectangle. The
// primary screen is the fallback for a rectangle that is not on any screen.
QScreen* screen_for_rect(const QRect& rect) {
    QScreen* best = nullptr;
    qint64 best_area = 0;
    for (QScreen* screen : QGuiApplication::screens()) {
        const QRect visible = rect.intersected(screen->availableGeometry());
        const qint64 area = static_cast<qint64>(visible.width()) * visible.height();
        if (area > best_area) {
            best_area = area;
            best = screen;
        }
    }
    return best ? best : QGuiApplication::primaryScreen();
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

    // Geometry writes are debounced so dragging or resizing does not rewrite
    // the config file for every pixel. The saved size is applied once the body
    // exists, because the layout's minimum size is only known then.
    geometry_save_timer_ = new QTimer(this);
    geometry_save_timer_->setSingleShot(true);
    geometry_save_timer_->setInterval(500);
    connect(geometry_save_timer_, &QTimer::timeout, this,
            &MainWindow::save_window_geometry);
    restore_window_geometry();

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
    // Flush a resize/move that is still inside the debounce window so the last
    // geometry survives a quit right after dragging the window.
    if (geometry_save_timer_) geometry_save_timer_->stop();
    save_window_geometry();
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

void MainWindow::restore_window_geometry() {
    const QRect saved(settings_.main_window_x, settings_.main_window_y,
                      settings_.main_window_width, settings_.main_window_height);
    QScreen* screen =
        settings_.has_main_window_position ? screen_for_rect(saved)
                                           : QGuiApplication::primaryScreen();
    if (!screen) {
        // No screen information (offscreen/headless run): keep the saved size.
        restoring_geometry_ = true;
        resize(settings_.main_window_width, settings_.main_window_height);
        restoring_geometry_ = false;
        return;
    }

    const QRect available = screen->availableGeometry();
    const QSize minimum = minimumSizeHint().expandedTo(QSize(320, 240));
    // A saved size larger than the screen (or below the layout minimum) is
    // reduced/clamped so the window stays usable.
    const int min_width = std::min(minimum.width(), available.width());
    const int min_height = std::min(minimum.height(), available.height());
    const int width =
        std::clamp(settings_.main_window_width, min_width, available.width());
    const int height =
        std::clamp(settings_.main_window_height, min_height, available.height());

    restoring_geometry_ = true;
    resize(width, height);
    if (settings_.has_main_window_position && window_rect_reachable(saved)) {
        // Keep part of the title bar inside the screen even when the saved
        // point was partially off-screen.
        const int visible_width =
            std::min({kWindowVisibleWidth, width, available.width()});
        const int visible_height =
            std::min({kWindowVisibleHeight, height, available.height()});
        const int min_x = available.left() - width + visible_width;
        const int max_x = std::max(min_x, available.right() - visible_width);
        const int min_y = available.top();
        const int max_y = std::max(min_y, available.bottom() - visible_height);
        const int x = std::clamp(settings_.main_window_x, min_x, max_x);
        const int y = std::clamp(settings_.main_window_y, min_y, max_y);
        move(x, y);
    }
    restoring_geometry_ = false;
}

void MainWindow::schedule_geometry_save() {
    if (restoring_geometry_ || !geometry_save_timer_) return;
    geometry_save_timer_->start();  // restarts the single-shot debounce
}

void MainWindow::save_window_geometry() {
    // A tray-only start that quits before the window was ever shown has no
    // placement to remember; saving it would pin the next start to (0, 0).
    if (!window_shown_) return;
    // The maximize state is not persisted. The normal geometry was already
    // saved by the debounce before the user maximized, so keep it instead of
    // overwriting it with screen coordinates.
    if (isMaximized() || isFullScreen()) return;
    // pos() is the window-frame position, which is what move() expects before
    // the window is created at startup, while geometry().topLeft() is the
    // client area. Saving the client position made every restore treat it as
    // a frame position and shift the window down by the title bar. The size
    // stays the client size that resize() expects.
    const QPoint top_left = pos();
    const QSize client_size = size();
    if (client_size.width() <= 0 || client_size.height() <= 0) return;
    if (settings_.main_window_width == client_size.width() &&
        settings_.main_window_height == client_size.height() &&
        settings_.has_main_window_position &&
        settings_.main_window_x == top_left.x() &&
        settings_.main_window_y == top_left.y()) {
        return;
    }
    settings_.main_window_width = client_size.width();
    settings_.main_window_height = client_size.height();
    settings_.main_window_x = top_left.x();
    settings_.main_window_y = top_left.y();
    settings_.has_main_window_position = true;
    std::string error;
    if (!swiftdm::save_settings(settings_, &error)) {
        qWarning() << "window geometry: cannot save settings:"
                   << QString::fromStdString(error);
    }
}

void MainWindow::resizeEvent(QResizeEvent* event) {
    QMainWindow::resizeEvent(event);
    schedule_geometry_save();
}

void MainWindow::moveEvent(QMoveEvent* event) {
    QMainWindow::moveEvent(event);
    schedule_geometry_save();
}

void MainWindow::showEvent(QShowEvent* event) {
    QMainWindow::showEvent(event);
    // Placement is only meaningful after the window manager has shown the
    // window; until then a tray-only run has nothing to save.
    window_shown_ = true;
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

void MainWindow::on_settings() {
    // Window-modal instead of exec(): the main window stays blocked, but other
    // top-level windows (Download Complete, progress windows) still take
    // clicks while the settings dialog is open.
    if (settings_dialog_) {
        settings_dialog_->raise();
        settings_dialog_->activateWindow();
        return;
    }
    auto* dialog = new SettingsDialog(settings_, this);
    dialog->setWindowModality(Qt::WindowModal);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    settings_dialog_ = dialog;
    connect(dialog, &QDialog::accepted, this, [this] {
        // The appearance settings take effect right away; no restart needed.
        apply_interface_settings();
        apply_tray_visibility();
    });
    dialog->open();
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
