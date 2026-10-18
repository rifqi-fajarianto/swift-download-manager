#include "ui/download_model.h"

#include <QBrush>
#include <QDateTime>
#include <QFont>

#include "core/util.h"
#include "engine/categories.h"
#include "ui/download_task.h"

namespace swiftdm::ui {

namespace {

QColor status_color(const DownloadItem& item) {
    switch (item.state) {
        case DownloadItem::State::Downloading:
        case DownloadItem::State::Paused:
            return QColor(0x5a, 0xa9, 0xe6);
        case DownloadItem::State::Complete:
            return QColor(0x7e, 0xc8, 0x7e);
        case DownloadItem::State::Error:
            return QColor(0xe0, 0x5a, 0x5a);
        case DownloadItem::State::Queued:
            break;
    }
    return QColor(0xb8, 0xb8, 0xb8);
}

QString format_size(quint64 bytes) {
    if (bytes == 0) return QString();
    return QString::fromStdString(swiftdm::format_bytes(bytes, 3));
}

}  // namespace

QString download_status_text(const DownloadItem& item) {
    switch (item.state) {
        case DownloadItem::State::Queued:
            return QObject::tr("Queued");
        case DownloadItem::State::Downloading:
            return QObject::tr("Downloading ( %1% )").arg(item.percent);
        case DownloadItem::State::Paused:
            return QObject::tr("Paused ( %1% )").arg(item.percent);
        case DownloadItem::State::Complete:
            return QObject::tr("Complete");
        case DownloadItem::State::Error:
            return QObject::tr("Error ( %1% )").arg(item.percent);
    }
    return QString();
}

DownloadModel::DownloadModel(QObject* parent) : QAbstractTableModel(parent) {}

int DownloadModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(items_.size());
}

int DownloadModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant DownloadModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= items_.size()) return QVariant();
    const DownloadItem& item = items_[index.row()];

    if (role == Qt::DisplayRole) {
        switch (index.column()) {
            case ColName:
                return item.file_name.isEmpty() ? item.url : item.file_name;
            case ColSize:
                return format_size(item.size == 0 ? item.done : item.size);
            case ColStatus:
                return download_status_text(item);
            case ColBandwidth:
                if (item.speed_bps <= 0.0) return QString();
                return QString::fromStdString(swiftdm::format_speed(item.speed_bps, 3));
            case ColRemaining:
                if (item.remaining_sec < 0) return QString();
                return QString::fromStdString(
                    swiftdm::format_duration(item.remaining_sec));
            case ColLastTry:
                return item.last_try;
            default:
                break;
        }
        return QVariant();
    }

    if (role == Qt::TextAlignmentRole) {
        if (index.column() == ColSize || index.column() == ColBandwidth ||
            index.column() == ColRemaining) {
            return static_cast<int>(Qt::AlignRight | Qt::AlignVCenter);
        }
        return static_cast<int>(Qt::AlignLeft | Qt::AlignVCenter);
    }

    if (role == Qt::ForegroundRole && index.column() == ColStatus) {
        return QBrush(status_color(item));
    }

    if (role == Qt::ToolTipRole) {
        QString tip = item.url;
        if (!item.error.isEmpty()) tip += "\n" + QObject::tr("Error: ") + item.error;
        if (!item.dest_path.isEmpty()) tip += "\n" + item.dest_path;
        return tip;
    }

    return QVariant();
}

QVariant DownloadModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (role != Qt::DisplayRole) return QVariant();
    if (orientation != Qt::Horizontal) {
        return static_cast<int>(section + 1);
    }
    switch (section) {
        case ColName: return tr("File Name");
        case ColSize: return tr("Size");
        case ColStatus: return tr("Status");
        case ColBandwidth: return tr("Bandwidth");
        case ColRemaining: return tr("Remaining Time");
        case ColLastTry: return tr("Last Try (z-a)");
        default: return QVariant();
    }
}

int DownloadModel::add_item(DownloadItem item) {
    item.id = next_id_++;
    const int row = static_cast<int>(items_.size());
    beginInsertRows(QModelIndex(), row, row);
    items_.append(item);
    endInsertRows();
    return row;
}

DownloadItem* DownloadModel::item_at(int row) {
    if (row < 0 || row >= items_.size()) return nullptr;
    return &items_[row];
}

const DownloadItem* DownloadModel::item_at(int row) const {
    if (row < 0 || row >= items_.size()) return nullptr;
    return &items_[row];
}

int DownloadModel::row_for_task(const DownloadTask* task) const {
    for (int i = 0; i < items_.size(); ++i) {
        if (items_[i].task == task) return i;
    }
    return -1;
}

int DownloadModel::row_for_id(int id) const {
    for (int i = 0; i < items_.size(); ++i) {
        if (items_[i].id == id) return i;
    }
    return -1;
}

void DownloadModel::remove_row(int row) {
    if (row < 0 || row >= items_.size()) return;
    beginRemoveRows(QModelIndex(), row, row);
    items_.remove(row);
    endRemoveRows();
}

void DownloadModel::touch_row(int row) {
    if (row < 0 || row >= items_.size()) return;
    emit dataChanged(index(row, 0), index(row, ColumnCount - 1));
}

void DownloadModel::task_probed(DownloadTask* task) {
    const int row = row_for_task(task);
    if (row < 0) return;
    DownloadItem& item = items_[row];
    const QString name = task->resolved_filename();
    if (!name.isEmpty()) {
        item.file_name = name;
        item.category =
            QString::fromStdString(swiftdm::category_folder_for(name.toStdString()));
    }
    if (task->resolved_size() > 0) item.size = task->resolved_size();
    item.resumable = task->resumable();
    emit dataChanged(index(row, 0), index(row, ColumnCount - 1));
}

void DownloadModel::task_progressed(DownloadTask* task) {
    const int row = row_for_task(task);
    if (row < 0) return;
    DownloadItem& item = items_[row];
    const DownloadTask::Snapshot snap = task->snapshot();
    item.done = snap.done;
    if (snap.total > 0) item.size = snap.total;
    item.percent = snap.percent;
    item.speed_bps = snap.speed_bps;
    if (item.size > item.done && snap.speed_bps > 1.0) {
        item.remaining_sec =
            static_cast<qint64>((item.size - item.done) / snap.speed_bps);
    } else {
        item.remaining_sec = 0;
    }
    item.state = DownloadItem::State::Downloading;
    emit dataChanged(index(row, 0), index(row, ColumnCount - 1));
}

void DownloadModel::task_finished(DownloadTask* task, int status, const QString& path,
                                  const QString& error) {
    const int row = row_for_task(task);
    if (row < 0) return;
    DownloadItem& item = items_[row];
    item.speed_bps = 0.0;
    item.remaining_sec = -1;
    if (status == static_cast<int>(DownloadTask::Status::Completed)) {
        item.state = DownloadItem::State::Complete;
        item.percent = 100;
        item.done = item.size;
        item.dest_path = path;
        item.error.clear();
    } else if (status == static_cast<int>(DownloadTask::Status::Paused)) {
        item.state = DownloadItem::State::Paused;
    } else if (status == static_cast<int>(DownloadTask::Status::Failed)) {
        item.state = DownloadItem::State::Error;
        item.error = error;
    }
    // Canceled rows are removed by the main window; leave the state untouched.
    emit dataChanged(index(row, 0), index(row, ColumnCount - 1));
}

DownloadFilterProxy::DownloadFilterProxy(QObject* parent) : QSortFilterProxyModel(parent) {}

void DownloadFilterProxy::set_category(const QString& category) {
    category_ = category;
    invalidateFilter();
}

void DownloadFilterProxy::set_completion_only(bool complete_only) {
    complete_only_ = complete_only;
    incomplete_only_ = false;
    invalidateFilter();
}

void DownloadFilterProxy::set_incomplete_only(bool incomplete_only) {
    incomplete_only_ = incomplete_only;
    complete_only_ = false;
    invalidateFilter();
}

bool DownloadFilterProxy::filterAcceptsRow(int source_row,
                                           const QModelIndex& source_parent) const {
    auto* model = qobject_cast<DownloadModel*>(sourceModel());
    if (!model) return true;
    const DownloadItem* item = model->item_at(source_row);
    if (!item) return true;
    (void)source_parent;
    if (!category_.isEmpty() && item->category != category_) return false;
    if (complete_only_ && !item->is_complete()) return false;
    if (incomplete_only_ && item->is_complete()) return false;
    return true;
}

}  // namespace swiftdm::ui
