#include "ui/download_window.h"

#include <QDateTime>
#include <QFileDialog>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>

#include "core/util.h"
#include "ui/download_model.h"
#include "ui/download_task.h"

namespace swiftdm::ui {

namespace {

QString format_bytes_q(quint64 bytes) {
    return QString::fromStdString(swiftdm::format_bytes(bytes, 3));
}

QString format_speed_q(double bps) {
    return QString::fromStdString(swiftdm::format_speed(bps, 3));
}

QString format_duration_q(qint64 seconds) {
    return QString::fromStdString(swiftdm::format_duration(seconds));
}

}  // namespace

// ---------------------------------------------------------------------------
// SegmentBar
// ---------------------------------------------------------------------------

SegmentBar::SegmentBar(QWidget* parent) : QWidget(parent) {
    setMinimumHeight(14);
}

QSize SegmentBar::sizeHint() const { return QSize(420, 16); }

void SegmentBar::set_segments(const QVector<QPair<quint64, quint64>>& segments) {
    segments_ = segments;
    update();
}

void SegmentBar::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), QColor(0x1c, 0x1c, 0x1c));
    if (segments_.isEmpty()) {
        painter.setPen(QColor(0x55, 0x55, 0x55));
        painter.drawRect(rect().adjusted(0, 0, -1, -1));
        return;
    }

    quint64 total = 0;
    for (const auto& segment : segments_) total += segment.second;
    if (total == 0) return;

    const int gap = 2;
    const double gaps = gap * std::max<int>(0, segments_.size() - 1);
    const double usable = std::max<double>(1.0, width() - gaps);
    double x = 0.0;
    for (const auto& segment : segments_) {
        const quint64 length = std::max<quint64>(1, segment.second);
        const quint64 done = std::min(segment.first, segment.second);
        const double slice_width = usable * static_cast<double>(length) /
                                   static_cast<double>(total);
        const QRectF slice(x, 0, std::max(1.0, slice_width), height());
        painter.fillRect(slice, QColor(0x2a, 0x5b, 0x8a));
        if (done > 0) {
            const double fraction = static_cast<double>(done) /
                                    static_cast<double>(length);
            painter.fillRect(QRectF(slice.x(), slice.y(), slice.width() * fraction,
                                    slice.height()),
                             QColor(0x6d, 0xbf, 0x6d));
        }
        x += slice_width + gap;
    }
    painter.setPen(QColor(0x55, 0x55, 0x55));
    painter.drawRect(rect().adjusted(0, 0, -1, -1));
}

// ---------------------------------------------------------------------------
// DownloadWindow
// ---------------------------------------------------------------------------

DownloadWindow::DownloadWindow(DownloadModel* model, int item_id,
                               swiftdm::Settings settings, QWidget* parent)
    : QDialog(parent),
      model_(model),
      item_id_(item_id),
      settings_(std::move(settings)) {
    setAttribute(Qt::WA_DeleteOnClose);
    resize(560, 420);

    auto* tabs = new QTabWidget(this);
    tabs->addTab(build_download_tab(), tr("Download"));
    tabs->addTab(build_options_tab(), tr("Options"));
    tabs->addTab(build_connections_tab(), tr("Connections"));

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(tabs, 1);

    connect(model_, &QAbstractItemModel::dataChanged, this, [this] { refresh(); });
    connect(model_, &QAbstractItemModel::rowsRemoved, this, [this] { refresh(); });
    connect(model_, &QAbstractItemModel::modelReset, this, [this] { refresh(); });
    refresh();
}

QWidget* DownloadWindow::build_download_tab() {
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    layout->setSpacing(6);

    url_label_ = new QLabel(page);
    url_label_->setOpenExternalLinks(true);
    url_label_->setTextInteractionFlags(Qt::TextBrowserInteraction);
    url_label_->setWordWrap(false);
    layout->addWidget(url_label_);
    layout->addSpacing(4);

    auto* grid = new QGridLayout;
    grid->setHorizontalSpacing(16);
    grid->setVerticalSpacing(4);
    const auto add_row = [&](int row, const QString& name, QLabel*& value) {
        grid->addWidget(new QLabel(name, page), row, 0, Qt::AlignLeft | Qt::AlignTop);
        value = new QLabel(page);
        value->setTextInteractionFlags(Qt::TextSelectableByMouse);
        grid->addWidget(value, row, 1, Qt::AlignLeft);
    };
    add_row(0, tr("Status"), status_label_);
    add_row(1, tr("File Size"), size_label_);
    add_row(2, tr("Downloaded"), downloaded_label_);
    add_row(3, tr("Bandwidth"), bandwidth_label_);
    add_row(4, tr("Remaining Time"), remaining_label_);
    add_row(5, tr("Resumable"), resumable_label_);
    grid->setColumnStretch(1, 1);
    layout->addLayout(grid);
    layout->addSpacing(8);

    progress_ = new QProgressBar(page);
    progress_->setRange(0, 100);
    progress_->setTextVisible(false);
    layout->addWidget(progress_);
    layout->addSpacing(6);

    segment_bar_ = new SegmentBar(page);
    layout->addWidget(segment_bar_);

    segments_label_ = new QLabel(page);
    segments_label_->setAlignment(Qt::AlignCenter);
    segments_label_->setStyleSheet("color:#4da3ff;");
    layout->addWidget(segments_label_);
    layout->addStretch(1);

    auto* buttons = new QHBoxLayout;
    buttons->addStretch(1);
    resume_button_ = new QPushButton(tr("Resume"), page);
    cancel_button_ = new QPushButton(tr("Cancel"), page);
    buttons->addWidget(resume_button_);
    buttons->addSpacing(28);
    buttons->addWidget(cancel_button_);
    buttons->addStretch(1);
    layout->addLayout(buttons);

    connect(resume_button_, &QPushButton::clicked, this, [this] {
        const int row = model_->row_for_id(item_id_);
        DownloadItem* item = model_->item_at(row);
        if (item && item->task && !item->task->active()) {
            item->state = DownloadItem::State::Downloading;
            item->last_try = QDateTime::currentDateTime().toString("MMM d HH:mm:ss");
            model_->touch_row(row);
            item->task->start();
        }
    });
    connect(cancel_button_, &QPushButton::clicked, this, [this] {
        const int row = model_->row_for_id(item_id_);
        DownloadItem* item = model_->item_at(row);
        if (item && item->task && item->task->active()) item->task->pause();
        close();
    });
    return page;
}

QWidget* DownloadWindow::build_options_tab() {
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    auto* form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

    dir_edit_ = new QLineEdit(page);
    name_edit_ = new QLineEdit(page);
    referer_edit_ = new QLineEdit(page);
    connections_spin_ = new QSpinBox(page);
    connections_spin_->setRange(0, 64);
    connections_spin_->setSpecialValueText(tr("default"));
    limit_spin_ = new QSpinBox(page);
    limit_spin_->setRange(0, 1024 * 1024);
    limit_spin_->setSpecialValueText(tr("default"));

    form->addRow(tr("Save To"), dir_edit_);
    form->addRow(tr("File Name"), name_edit_);
    form->addRow(tr("Connections"), connections_spin_);
    form->addRow(tr("Bandwidth Limit (KB/s)"), limit_spin_);
    form->addRow(tr("Referer"), referer_edit_);
    layout->addLayout(form);

    auto* note = new QLabel(tr("Changes apply when the download is resumed."), page);
    note->setStyleSheet("color:#a0a0a0;");
    layout->addWidget(note);
    layout->addStretch(1);

    apply_button_ = new QPushButton(tr("Apply"), page);
    auto* buttons = new QHBoxLayout;
    buttons->addStretch(1);
    buttons->addWidget(apply_button_);
    layout->addLayout(buttons);
    connect(apply_button_, &QPushButton::clicked, this, [this] {
        const int row = model_->row_for_id(item_id_);
        DownloadItem* item = model_->item_at(row);
        DownloadTask* task = item ? item->task : nullptr;
        if (!task || task->active()) return;
        DownloadTask::Options options = task->options();
        options.output_dir = dir_edit_->text().trimmed();
        options.filename = name_edit_->text().trimmed();
        options.connections = connections_spin_->value();
        options.limit_kbps = limit_spin_->value();
        options.referer = referer_edit_->text().trimmed();
        task->set_options(options);
    });
    return page;
}

QWidget* DownloadWindow::build_connections_tab() {
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    connections_table_ = new QTableWidget(0, 4, page);
    connections_table_->setHorizontalHeaderLabels(
        {tr("#"), tr("Range (bytes)"), tr("Downloaded"), tr("%")});
    connections_table_->horizontalHeader()->setStretchLastSection(true);
    connections_table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    connections_table_->verticalHeader()->setVisible(false);
    connections_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    connections_table_->setSelectionMode(QAbstractItemView::NoSelection);
    layout->addWidget(connections_table_);
    return page;
}

void DownloadWindow::refresh() {
    const int row = model_->row_for_id(item_id_);
    if (row < 0) {
        close();
        return;
    }
    const DownloadItem* item = model_->item_at(row);
    if (!item) {
        close();
        return;
    }

    const QString title =
        item->file_name.isEmpty() ? tr("Download") : item->file_name;
    setWindowTitle(QString("%1 - %2").arg(download_status_text(*item), title));
    url_label_->setText(QString("<a href=\"%1\">%1</a>").arg(item->url.toHtmlEscaped()));
    status_label_->setText(download_status_text(*item));
    size_label_->setText(item->size > 0 ? format_bytes_q(item->size) : tr("Unknown"));
    downloaded_label_->setText(
        QString("%1 ( %2% )").arg(format_bytes_q(item->done)).arg(item->percent));
    bandwidth_label_->setText(item->speed_bps > 0 ? format_speed_q(item->speed_bps)
                                                  : QString());
    remaining_label_->setText(item->remaining_sec >= 0
                                  ? format_duration_q(item->remaining_sec)
                                  : QString());
    resumable_label_->setText(item->resumable ? tr("Yes") : tr("No"));
    resumable_label_->setStyleSheet(item->resumable ? "color:#7ec87e;" : "color:#e05a5a;");
    progress_->setValue(item->percent);

    const DownloadTask* task = item->task;
    const DownloadTask::Snapshot snap = task ? task->snapshot() : DownloadTask::Snapshot{};
    segment_bar_->set_segments(snap.segments);
    segments_label_->setText(tr("Segments : %1").arg(snap.segments.size()));

    const bool active = task && task->active();
    const bool can_resume = !active && (item->state == DownloadItem::State::Paused ||
                                        item->state == DownloadItem::State::Error ||
                                        item->state == DownloadItem::State::Queued);
    resume_button_->setEnabled(can_resume);
    apply_button_->setEnabled(!active);

    if (!active && dir_edit_->text().isEmpty()) {
        QString dir = QString::fromStdString(settings_.download_dir);
        if (settings_.category_folders && !item->category.isEmpty()) {
            dir += "/" + item->category;
        }
        dir_edit_->setText(dir);
        name_edit_->setText(item->file_name);
        connections_spin_->setValue(task ? task->options().connections : 0);
        limit_spin_->setValue(task ? task->options().limit_kbps : 0);
        referer_edit_->setText(task ? task->options().referer : QString());
    }
    refresh_connections();
}

void DownloadWindow::refresh_connections() {
    const int row = model_->row_for_id(item_id_);
    const DownloadItem* item = model_->item_at(row);
    const DownloadTask* task = item ? item->task : nullptr;
    const DownloadTask::Snapshot snap = task ? task->snapshot() : DownloadTask::Snapshot{};
    connections_table_->setRowCount(static_cast<int>(snap.segments.size()));
    quint64 offset = 0;
    for (int i = 0; i < snap.segments.size(); ++i) {
        const quint64 done = snap.segments[i].first;
        const quint64 length = snap.segments[i].second;
        const auto set = [&](int column, const QString& text) {
            auto* item = connections_table_->item(i, column);
            if (!item) {
                item = new QTableWidgetItem;
                connections_table_->setItem(i, column, item);
            }
            item->setText(text);
        };
        set(0, QString::number(i + 1));
        set(1, length > 0 ? QString("%1 - %2")
                                .arg(offset)
                                .arg(offset + length - 1)
                          : QString("-"));
        set(2, format_bytes_q(done));
        set(3, length > 0 ? QString::number(100 * done / length) : QString("0"));
        offset += length;
    }
}

}  // namespace swiftdm::ui
