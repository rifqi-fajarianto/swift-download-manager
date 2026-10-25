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

}  // namespace swiftdm::ui
