#pragma once

#include <QAbstractTableModel>
#include <QColor>
#include <QSortFilterProxyModel>
#include <QString>
#include <QVector>

namespace swiftdm::ui {

class DownloadTask;

struct DownloadItem {
    enum class State { Queued, Probing, Downloading, Paused, Complete, Error };

    int id = 0;
    QString url;
    QString file_name;
    QString category;
    QString output_dir;
    quint64 size = 0;   // 0 = unknown
    quint64 done = 0;
    State state = State::Queued;
    int percent = 0;
    double speed_bps = 0.0;
    qint64 remaining_sec = -1;
    QString last_try;
    qint64 last_try_epoch = 0;  // unix seconds, used for sorting
    QString added;  // filled in by DownloadModel::add_item
    QString error;
    QString dest_path;
    QString temp_path;   // workspace kept for resume (restored from history)
    QString page_url;    // browser page that initiated the download
    QString page_title;  // title of that page
    QString group;       // adaptive pair token (video + audio), empty otherwise
    qint64 history_id = 0;  // row id in the SQLite history (0 = not stored)
    bool resumable = false;
    // Per-download speed limiter (mirrors the task options/history row so the
    // progress window can show them for restored/completed rows too).
    bool bandwidth_limit_enabled = false;
    int bandwidth_limit_kbps = 0;
    // Complete download whose file is no longer at dest_path (deleted/moved).
    bool file_missing = false;
    DownloadTask* task = nullptr;

    bool is_complete() const { return state == State::Complete; }
};

// Toolbar availability for the Resume and Stop actions. Resume starts a row
// that is not running (paused, failed or queued); Stop pauses an active
// transfer. A mixed selection enables an action when at least one selected row
// can use it.
struct ToolbarActionState {
    bool resume = false;
    bool stop = false;
};

inline bool item_can_resume(DownloadItem::State state, bool has_task, bool task_active) {
    if (!has_task || task_active) return false;
    return state == DownloadItem::State::Paused ||
           state == DownloadItem::State::Error ||
           state == DownloadItem::State::Queued;
}

inline bool item_can_stop(DownloadItem::State state, bool has_task, bool task_active) {
    return has_task && task_active && state == DownloadItem::State::Downloading;
}

inline void merge_toolbar_action_state(ToolbarActionState& actions,
                                       DownloadItem::State state, bool has_task,
                                       bool task_active) {
    actions.resume = actions.resume || item_can_resume(state, has_task, task_active);
    actions.stop = actions.stop || item_can_stop(state, has_task, task_active);
}

// Status text shown in the table and download window ("Paused ( 54% )" etc.).
QString download_status_text(const DownloadItem& item);

// Warning shown when a completed download's file is gone from disk.
QString missing_file_warning(const QString& path);

// Status text color used in the table and the properties window.
QColor download_status_color(const DownloadItem& item);

// Table model for the main window (columns per docs/spec/01-product.md).
class DownloadModel : public QAbstractTableModel {
    Q_OBJECT
  public:
    enum Column {
        ColName = 0,
        ColSize,
        ColStatus,
        ColBandwidth,
        ColRemaining,
        ColLastTry,
        ColumnCount,
    };

    explicit DownloadModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

    int add_item(DownloadItem item);  // returns the new row
    DownloadItem* item_at(int row);
    const DownloadItem* item_at(int row) const;
    int row_for_task(const DownloadTask* task) const;
    int row_for_id(int id) const;
    void remove_row(int row);
    void touch_row(int row);  // emits dataChanged for the whole row

    // Slot-ish helpers wired to DownloadTask signals.
    void task_probed(DownloadTask* task);
    void task_progressed(DownloadTask* task);
    void task_finished(DownloadTask* task, int status, const QString& path,
                       const QString& error);

  private:
    QVector<DownloadItem> items_;
    int next_id_ = 1;
};

// Filters the table by category folder and completion state.
class DownloadFilterProxy : public QSortFilterProxyModel {
    Q_OBJECT
  public:
    explicit DownloadFilterProxy(QObject* parent = nullptr);

    void set_category(const QString& category);  // empty = all
    void set_completion_only(bool complete_only);
    void set_incomplete_only(bool incomplete_only);

  protected:
    bool filterAcceptsRow(int source_row, const QModelIndex& source_parent) const override;

  private:
    QString category_;
    bool complete_only_ = false;
    bool incomplete_only_ = false;
};

}  // namespace swiftdm::ui
