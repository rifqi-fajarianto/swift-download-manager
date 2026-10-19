#pragma once

#include <QHash>
#include <QJsonObject>
#include <QMainWindow>
#include <QPoint>
#include <QPointer>

#include "store/history.h"
#include "store/settings.h"
#include "ui/download_model.h"
#include "ui/download_task.h"

class QAction;
class QCloseEvent;
class QLabel;
class QModelIndex;
class QSystemTrayIcon;
class QTableView;
class QTreeWidget;

namespace swiftdm::ui {

class DownloadFilterProxy;
class DownloadModel;
class DownloadWindow;
class BrowserIntegration;

// Main window: toolbar, category tree, downloads table, status bar and tray
// (layout per docs/spec/01-product.md §2).
class MainWindow : public QMainWindow {
    Q_OBJECT
  public:
    explicit MainWindow(swiftdm::Settings settings, QWidget* parent = nullptr);
    ~MainWindow() override;

  protected:
    void closeEvent(QCloseEvent* event) override;

  private slots:
    void on_new_url();
    void on_resume();
    void on_stop();
    void on_delete();
    void on_settings();
    void on_browsers();
    void on_about();
    void on_quit();
    void on_tree_selection();
    void on_row_activated(const QModelIndex& index);
    void on_context_menu(const QPoint& pos);
    void on_delete_file();

  private:
    void build_actions();
    void build_body();
    void build_tray();
    void update_status_bar();
    void add_download(const QString& url);
    void add_browser_download(const QJsonObject& data);
    void start_task_at_row(int row, const DownloadTask::Options* options_override = nullptr);
    void start_probe_at_row(int row);
    void on_probe_completed(DownloadTask* task, bool ok, const QString& error);
    void open_download_window(int id);
    void on_task_finished(DownloadTask* task, int status, const QString& path,
                          const QString& error);
    void show_completion_dialog(const QString& name, const QString& path);
    void show_error_dialog(const QString& url, const QString& error,
                           const QString& page_url = QString());
    // Restores the download list from the SQLite history at startup.
    void restore_history();
    // Rebuilds the task options of a restored row (no transfer is started).
    DownloadTask::Options options_from_record(const swiftdm::HistoryStore::Record& record) const;
    // Connects a task's signals to the model/main window (used for new and
    // restored tasks; restored tasks are wired but never started).
    void wire_task(DownloadTask* task);
    // Throttled progress write for the running task (history survives a crash).
    void persist_progress(DownloadTask* task);
    // Re-checks whether a completed item's file is still on disk.
    void refresh_missing_state(int row);
    void show_missing_files_dialog(const QStringList& paths);
    void show_missing_file_dialog(const DownloadItem& item);
    void open_file_for_row(int row);
    void open_folder_for_row(int row);
    void show_properties_for_row(int row);
    void remove_row_with_cleanup(int row);
    void update_action_states();
    int row_at_view_pos(const QPoint& pos) const;  // source row, or -1
    QList<int> selected_rows() const;  // source rows, descending

    swiftdm::Settings settings_;
    swiftdm::HistoryStore history_;
    DownloadModel* model_ = nullptr;
    DownloadFilterProxy* proxy_ = nullptr;
    QTableView* table_ = nullptr;
    QTreeWidget* tree_ = nullptr;
    QLabel* status_label_ = nullptr;
    QSystemTrayIcon* tray_ = nullptr;
    BrowserIntegration* browser_integration_ = nullptr;

    QAction* act_new_ = nullptr;
    QAction* act_resume_ = nullptr;
    QAction* act_stop_ = nullptr;
    QAction* act_delete_ = nullptr;
    QAction* act_delete_file_ = nullptr;
    QAction* act_settings_ = nullptr;
    QAction* act_browsers_ = nullptr;
    QAction* act_about_ = nullptr;
    QAction* act_quit_ = nullptr;

    QHash<int, QPointer<DownloadWindow>> windows_;
    QHash<qint64, qint64> last_progress_save_ms_;  // history id -> last DB write
    bool quitting_ = false;
    bool tray_note_shown_ = false;
};

}  // namespace swiftdm::ui
