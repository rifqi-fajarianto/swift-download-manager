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

}  // namespace swiftdm::ui
