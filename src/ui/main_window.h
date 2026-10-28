#pragma once

#include <QHash>
#include <QJsonObject>
#include <QMainWindow>
#include <QPoint>
#include <QPointer>
#include <QSet>
#include <QVector>

#include "store/history.h"
#include "store/settings.h"
#include "ui/confirm_download_dialog.h"
#include "ui/download_model.h"
#include "ui/download_task.h"
#include "ui/queue_logic.h"

class QAction;
class QCloseEvent;
class QDialog;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QMessageBox;
class QModelIndex;
class QPushButton;
class QStackedWidget;
class QSystemTrayIcon;
class QTableView;
class QTimer;
class QToolBar;
class QTreeWidget;
class QTreeWidgetItem;

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

    // True when a tray icon exists, i.e. the window can stay hidden at login.
    bool has_tray_icon() const { return tray_ != nullptr; }

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
    void on_new_queue();
    void on_delete_queue();
    void on_start_queue();
    void on_pause_queue();
    void on_queue_properties();
    void on_tree_context_menu(const QPoint& pos);
    void on_queue_overview_clicked(QListWidgetItem* item);
    void on_shutdown_tick();

  private:
    void build_actions();
    void build_body();
    void build_tray();
    void update_status_bar();
    // Applies the appearance settings (theme, toolbar, tray icon) after the
    // settings dialog was accepted.
    void apply_interface_settings();
    void apply_toolbar_settings();
    void apply_tray_settings();
    // Creates or removes the tray icon after the setting changed.
    void apply_tray_visibility();
    // Re-tints the toolbar icons; the neutral tint depends on the theme.
    void refresh_action_icons();
    void add_download(const QString& url, qint64 queue_id = 0);
    void add_browser_download(const QJsonObject& data);
    // Probes the URL (no list row, no database entry) and shows the download
    // confirmation dialog. Only Download Now inserts the item and starts it.
    void probe_and_confirm(DownloadTask::Options options, DownloadItem item);
    void start_confirmed_download(DownloadItem item, DownloadTask::Options options,
                                  const swiftdm::ProbeResult& probe);
    void start_task_at_row(int row, const DownloadTask::Options* options_override = nullptr,
                           const swiftdm::ProbeResult* probe = nullptr);
    void open_download_window(int id);
    void on_task_finished(DownloadTask* task, int status, const QString& path,
                          const QString& error);
    void show_completion_dialog(const QString& name, const QString& path);
    // Opens the folder of a finished download (General tab setting).
    void open_download_folder(const QString& path);
    // Asks the user whether the app really closes; shared by the Quit action
    // and the close event.
    bool confirm_quit();
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
    void show_missing_file_dialog(const DownloadItem& item);
    void open_file_for_row(int row);
    void open_folder_for_row(int row);
    void show_properties_for_row(int row);
    void remove_row_with_cleanup(int row, bool check_queue = true);
    void remove_download_log(const DownloadItem* item);
    void update_action_states();
    int row_at_view_pos(const QPoint& pos) const;  // source row, or -1
    QList<int> selected_rows() const;  // source rows, descending

    // Queue support. Queues are workspaces over the history table; their
    // scheduler only touches rows that are not active, so the main list keeps
    // working unchanged.
    void load_queues();
    void rebuild_queue_tree(qint64 select_id = -1);
    void refresh_queue_overview();
    void update_queue_action_visibility();
    const swiftdm::HistoryStore::QueueRecord* queue_for(qint64 id) const;
    swiftdm::HistoryStore::QueueRecord* mutable_queue_for(qint64 id);
    QVector<QueueChoice> queue_choices() const;
    int queue_item_count(qint64 queue_id) const;
    int row_for_history(qint64 history_id) const;
    std::vector<QueueItemView> queue_views(qint64 queue_id) const;
    void start_queue(qint64 queue_id);
    void pause_queue(qint64 queue_id);
    void schedule_queue(qint64 queue_id);
    void enforce_queue_limit(qint64 queue_id);
    void apply_queue_speed_limits(qint64 queue_id);
    int queue_share_for_new_task(qint64 queue_id) const;
    // Manual starts (context menu, progress window) still honor the queue's
    // simultaneous limit; Start Queue and the scheduler manage slots
    // themselves and do not call this.
    bool allow_queue_item_start(qint64 queue_id);
    void check_queue_completion(qint64 queue_id);
    void cancel_queue_shutdown(qint64 queue_id, bool user_cancelled = false);
    void show_shutdown_countdown(qint64 queue_id);
    void update_shutdown_text();
    void perform_shutdown(qint64 queue_id);
    void notify_queue(qint64 queue_id, const QString& title, const QString& text);
    void move_item_to_queue(int row, qint64 queue_id);
    void move_item_to_downloads(int row);
    void move_selected_to_downloads();
    void open_queue_properties(qint64 queue_id);
    void rename_queue(qint64 queue_id);
    void delete_queue(qint64 queue_id);
    void start_autostart_queues();
    bool queue_features_available() const { return history_.is_open(); }

    swiftdm::Settings settings_;
    swiftdm::HistoryStore history_;
    DownloadModel* model_ = nullptr;
    DownloadFilterProxy* proxy_ = nullptr;
    QStackedWidget* right_stack_ = nullptr;
    QTableView* table_ = nullptr;
    QWidget* queue_overview_ = nullptr;
    QStackedWidget* queue_pages_ = nullptr;
    QListWidget* queue_list_ = nullptr;
    QLabel* queue_title_ = nullptr;
    QLabel* queue_empty_label_ = nullptr;
    QPushButton* queue_empty_button_ = nullptr;
    QTreeWidget* tree_ = nullptr;
    QLabel* status_label_ = nullptr;
    QToolBar* toolbar_ = nullptr;
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

    // Queue toolbar actions and the separators that keep the two toolbars
    // (Downloads and one queue) tidy.
    QAction* act_queue_add_ = nullptr;
    QAction* act_start_queue_ = nullptr;
    QAction* act_pause_queue_ = nullptr;
    QAction* act_queue_props_ = nullptr;
    QAction* act_new_queue_ = nullptr;
    QAction* act_delete_queue_ = nullptr;
    QAction* sep_after_new_ = nullptr;
    QAction* sep_download_actions_ = nullptr;
    QAction* sep_queue_actions_ = nullptr;
    QAction* sep_queue_transfer_actions_ = nullptr;

    QVector<swiftdm::HistoryStore::QueueRecord> queues_;
    qint64 selected_queue_id_ = 0;  // 0 = no queue (main list or overview)
    bool queue_mode_ = false;       // the Queues branch or a queue is selected
    QSet<qint64> running_queues_;
    // Items paused by the queue limit (not by the user). When they report
    // Paused they return to Queued if their queue is still running.
    QSet<int> limit_paused_items_;
    QSet<qint64> complete_notified_;
    QTreeWidgetItem* queues_root_ = nullptr;
    QTreeWidgetItem* all_item_ = nullptr;

    ShutdownScheduler shutdown_scheduler_;
    QTimer* shutdown_timer_ = nullptr;
    QMessageBox* shutdown_box_ = nullptr;
    qint64 shutdown_queue_id_ = 0;
    bool shutting_down_ = false;

    QHash<int, QPointer<DownloadWindow>> windows_;
    // Kept so a second Settings request raises the open window instead of
    // stacking another one.
    QPointer<QDialog> settings_dialog_;
    QHash<qint64, qint64> last_progress_save_ms_;  // history id -> last DB write
    bool quitting_ = false;
    bool tray_note_shown_ = false;
};

}  // namespace swiftdm::ui
