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

}  // namespace swiftdm::ui
