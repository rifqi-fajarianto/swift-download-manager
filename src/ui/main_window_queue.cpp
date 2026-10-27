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

}  // namespace swiftdm::ui
