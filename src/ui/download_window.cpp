#include "ui/download_window.h"

#include <QFileDialog>
#include <QFontMetrics>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QResizeEvent>
#include <QSizePolicy>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

#include "core/util.h"
#include "ui/download_model.h"
#include "ui/download_task.h"
#include "ui/icons.h"
#include "ui/speed_limit_editor.h"
#include "ui/section_box.h"
#include "ui/theme.h"

namespace swiftdm::ui {

namespace {

// Fixed default size for the progress window: independent of the URL length,
// the file name, or the current tab (docs/spec/01-product.md §3.2).
constexpr int kDefaultWindowWidth = 650;
constexpr int kDefaultWindowHeight = 500;
constexpr int kMinWindowWidth = 520;
constexpr int kMinWindowHeight = 420;
constexpr int kMinUrlLabelWidth = 120;
// Dialog chrome around the link row (window/tab margins, icon, spacings).
constexpr int kUrlLabelChrome = 80;

QLabel* add_value_row(QFormLayout* form, const QString& label, QLabel*& value) {
    value = new QLabel(form->parentWidget());
    value->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow(label, value);
    return value;
}

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
    // The bar is drawn by hand, so it has to follow the theme itself.
    const bool dark = is_dark_theme();
    painter.fillRect(rect(), dark ? QColor(0x1c, 0x1c, 0x1c) : QColor(0xe4, 0xe4, 0xe4));
    const QColor border = dark ? QColor(0x55, 0x55, 0x55) : QColor(0xa8, 0xa8, 0xa8);
    if (segments_.isEmpty()) {
        painter.setPen(border);
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
        painter.fillRect(slice, dark ? QColor(0x2a, 0x5b, 0x8a) : QColor(0x3a, 0x74, 0xb0));
        if (done > 0) {
            const double fraction = static_cast<double>(done) /
                                    static_cast<double>(length);
            painter.fillRect(QRectF(slice.x(), slice.y(), slice.width() * fraction,
                                    slice.height()),
                             dark ? QColor(0x6d, 0xbf, 0x6d) : QColor(0x3f, 0x8f, 0x3f));
        }
        x += slice_width + gap;
    }
    painter.setPen(border);
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
    setMinimumSize(kMinWindowWidth, kMinWindowHeight);
    resize(kDefaultWindowWidth, kDefaultWindowHeight);

    auto* tabs = new QTabWidget(this);
    tabs->addTab(build_download_tab(), tr("Download"));
    tabs->addTab(build_options_tab(), tr("Options"));
    tabs->addTab(build_connections_tab(), tr("Connections"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->addWidget(tabs, 1);

    connect(model_, &QAbstractItemModel::dataChanged, this, [this] { refresh(); });
    connect(model_, &QAbstractItemModel::rowsRemoved, this, [this] { refresh(); });
    connect(model_, &QAbstractItemModel::modelReset, this, [this] { refresh(); });
    refresh();
}

QWidget* DownloadWindow::build_download_tab() {
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->setSpacing(kSectionSpacing);

    url_label_ = new QLabel(page);
    url_label_->setOpenExternalLinks(true);
    url_label_->setTextInteractionFlags(Qt::TextBrowserInteraction);
    url_label_->setWordWrap(false);
    // A long URL must not drive the window width: the label ignores its own
    // size hint horizontally and update_url_label() elides the shown text.
    url_label_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    url_label_->setMinimumWidth(0);
    auto* url_row = new QHBoxLayout;
    auto* link_icon = new QLabel(page);
    link_icon->setPixmap(
        tinted_icon(QStringLiteral(":/icons/link.svg"), colors::kActionBlue).pixmap(16, 16));
    link_icon->setFixedSize(16, 16);
    url_row->addWidget(link_icon);
    url_row->addWidget(url_label_, 1);
    layout->addLayout(url_row);

    auto* details = add_form_section(layout, page, tr("Details"));
    add_value_row(details, tr("Status"), status_label_)->setObjectName("download_status");
    add_value_row(details, tr("File size"), size_label_)->setObjectName("download_file_size");
    add_value_row(details, tr("Downloaded"), downloaded_label_)
        ->setObjectName("download_downloaded");

    auto* bandwidth_row = new QWidget(page);
    auto* bandwidth_layout = new QHBoxLayout(bandwidth_row);
    bandwidth_layout->setContentsMargins(0, 0, 0, 0);
    bandwidth_layout->setSpacing(8);
    bandwidth_label_ = new QLabel(bandwidth_row);
    bandwidth_label_->setObjectName("download_bandwidth");
    bandwidth_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    // Active speed-limiter info sits next to the bandwidth value, in orange.
    limit_info_label_ = new QLabel(bandwidth_row);
    limit_info_label_->setObjectName("download_limit_info");
    limit_info_label_->setStyleSheet("color:#e0a85a;");
    limit_info_label_->setToolTip(tr("Speed limiter is active for this download"));
    bandwidth_layout->addWidget(bandwidth_label_);
    bandwidth_layout->addWidget(limit_info_label_);
    bandwidth_layout->addStretch(1);
    details->addRow(tr("Bandwidth"), bandwidth_row);

    add_value_row(details, tr("Remaining time"), remaining_label_)
        ->setObjectName("download_remaining");
    add_value_row(details, tr("Resumable"), resumable_label_)
        ->setObjectName("download_resumable");

    auto* progress_section = add_section(layout, page, tr("Progress"));
    progress_ = new QProgressBar(page);
    progress_->setObjectName("download_progress");
    progress_->setRange(0, 100);
    progress_->setTextVisible(true);
    progress_section->addWidget(progress_);

    segment_bar_ = new SegmentBar(page);
    segment_bar_->setObjectName("download_segment_bar");
    progress_section->addWidget(segment_bar_);

    segments_label_ = new QLabel(page);
    segments_label_->setObjectName("download_segments");
    segments_label_->setAlignment(Qt::AlignCenter);
    segments_label_->setStyleSheet("color:#4da3ff;");
    progress_section->addWidget(segments_label_);
    layout->addStretch(1);

    auto* buttons = new QHBoxLayout;
    buttons->setSpacing(8);
    buttons->addStretch(1);
    action_button_ = new QPushButton(tr("Pause"), page);  // label toggles in refresh()
    action_button_->setObjectName("download_action");
    cancel_button_ = new QPushButton(tr("Cancel"), page);
    cancel_button_->setObjectName("download_cancel");
    buttons->addWidget(action_button_);
    buttons->addWidget(cancel_button_);
    layout->addLayout(buttons);

    connect(action_button_, &QPushButton::clicked, this, [this] {
        const int row = model_->row_for_id(item_id_);
        DownloadItem* item = model_->item_at(row);
        if (!item || !item->task) return;
        DownloadTask* task = item->task;
        if (task->active()) {
            if (item->state == DownloadItem::State::Downloading) task->pause();
        } else if (item->state == DownloadItem::State::Paused ||
                   item->state == DownloadItem::State::Error ||
                   item->state == DownloadItem::State::Queued) {
            // The main window rebuilds the task so the SQLite history row is
            // updated (status, last try) as well.
            emit resumeRequested(item_id_);
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
    layout->setContentsMargins(10, 10, 10, 10);
    layout->setSpacing(kSectionSpacing);

    dir_edit_ = new QLineEdit(page);
    dir_edit_->setObjectName("download_save_to");
    name_edit_ = new QLineEdit(page);
    name_edit_->setObjectName("download_file_name");
    referer_edit_ = new QLineEdit(page);
    referer_edit_->setObjectName("download_referer");
    connections_spin_ = new QSpinBox(page);
    connections_spin_->setObjectName("download_connections");
    connections_spin_->setRange(0, 64);
    connections_spin_->setSpecialValueText(tr("default"));
    limit_editor_ = new SpeedLimitEditor(page);
    limit_editor_->setObjectName("download_bandwidth_limit");
    limit_debounce_ = new QTimer(this);
    limit_debounce_->setSingleShot(true);
    limit_debounce_->setInterval(500);
    connect(limit_editor_, &SpeedLimitEditor::valueEdited, this,
            [this](bool, int) {
                const int row = model_->row_for_id(item_id_);
                const DownloadItem* item = model_->item_at(row);
                if (item && item->task) limit_debounce_->start();
            });
    connect(limit_debounce_, &QTimer::timeout, this, [this] { apply_live_limit(); });

    auto* destination = add_form_section(layout, page, tr("Destination"));
    destination->addRow(tr("Save to:"), dir_edit_);
    destination->addRow(tr("File name:"), name_edit_);

    auto* transfer = add_section(layout, page, tr("Transfer"));
    auto* transfer_form = new QFormLayout;
    transfer_form->setContentsMargins(0, 0, 0, 0);
    transfer_form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    transfer_form->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);
    transfer_form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    transfer_form->setHorizontalSpacing(14);
    transfer_form->setVerticalSpacing(6);
    transfer_form->addRow(tr("Connections:"), connections_spin_);
    transfer_form->addRow(tr("Bandwidth limit:"), limit_editor_);
    transfer_form->addRow(tr("Referer:"), referer_edit_);
    transfer->addLayout(transfer_form);

    auto* note = new QLabel(
        tr("Speed limit changes apply while downloading. Other changes apply when resuming."),
        page);
    note->setObjectName("download_transfer_note");
    note->setStyleSheet("color:#a0a0a0;");
    note->setWordWrap(true);
    transfer->addWidget(note);
    layout->addStretch(1);

    apply_button_ = new QPushButton(tr("Apply"), page);
    apply_button_->setObjectName("download_apply");
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
        // The field shows the exact folder for this download; keep the engine
        // from appending a category subfolder again.
        options.apply_category_folder = false;
        // The name becomes a file on disk, so it is sanitized and shortened
        // here as well: the table then shows the name that is really used.
        options.filename = QString::fromStdString(swiftdm::limit_filename_length(
            swiftdm::sanitize_filename(name_edit_->text().trimmed().toStdString())));
        options.connections = connections_spin_->value();
        options.limit_enabled = limit_editor_->enabled();
        options.limit_kbps = limit_editor_->kbps();
        options.referer = referer_edit_->text().trimmed();
        task->set_options(options);
        emit optionsApplied(item_id_);
    });
    return page;
}

QWidget* DownloadWindow::build_connections_tab() {
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->setSpacing(kSectionSpacing);
    auto* frame = add_section(layout, page, QString());
    connections_table_ = new QTableWidget(0, 4, page);
    connections_table_->setObjectName("download_connections_table");
    connections_table_->setFrameShape(QFrame::NoFrame);
    connections_table_->setShowGrid(false);
    connections_table_->setHorizontalHeaderLabels(
        {tr("#"), tr("Range (bytes)"), tr("Downloaded"), tr("%")});
    connections_table_->horizontalHeader()->setStretchLastSection(true);
    connections_table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    connections_table_->verticalHeader()->setVisible(false);
    connections_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    connections_table_->setSelectionMode(QAbstractItemView::NoSelection);
    frame->addWidget(connections_table_);
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

    // Close automatically once the transfer completes while this window is
    // open; the completion notification is shown separately by the main window.
    if (item->state == DownloadItem::State::Complete) {
        if (seen_incomplete_) {
            close();
            return;
        }
    } else {
        seen_incomplete_ = true;
    }

    const QString title =
        item->file_name.isEmpty() ? tr("Download") : item->file_name;
    setWindowTitle(QString("%1 - %2").arg(download_status_text(*item), title));
    url_text_ = item->url;
    update_url_label();
    status_label_->setText(download_status_text(*item));
    size_label_->setText(item->size > 0 ? format_bytes_q(item->size) : tr("Unknown"));
    downloaded_label_->setText(
        QString("%1 ( %2% )").arg(format_bytes_q(item->done)).arg(item->percent));
    bandwidth_label_->setText(item->speed_bps > 0 ? format_speed_q(item->speed_bps)
                                                  : QString());
    if (item->bandwidth_limit_enabled && item->bandwidth_limit_kbps > 0) {
        limit_info_label_->setText(
            tr("Speed limit: %1").arg(format_speed_limit(item->bandwidth_limit_kbps)));
    } else {
        limit_info_label_->clear();
    }
    remaining_label_->setText(item->remaining_sec >= 0
                                  ? format_duration_q(item->remaining_sec)
                                  : QString());
    resumable_label_->setText(item->resumable ? tr("Yes") : tr("No"));
    resumable_label_->setStyleSheet(item->resumable ? "color:#7ec87e;" : "color:#e05a5a;");
    progress_->setValue(item->percent);

    const DownloadTask* task = item->task;
    const DownloadTask::Snapshot snap = task ? task->snapshot() : DownloadTask::Snapshot{};
    segment_bar_->set_segments(snap.segments);
    segments_label_->setText(tr("Segments: %1").arg(snap.segments.size()));

    const bool active = task && task->active();
    const bool running = active && item->state == DownloadItem::State::Downloading;
    const bool can_resume = !active && (item->state == DownloadItem::State::Paused ||
                                        item->state == DownloadItem::State::Error ||
                                        item->state == DownloadItem::State::Queued);
    action_button_->setText(running ? tr("Pause") : tr("Resume"));
    action_button_->setEnabled(running || can_resume);
    apply_button_->setEnabled(!active && task != nullptr);
    const bool other_options_enabled = !active && item->state != DownloadItem::State::Complete;
    dir_edit_->setEnabled(other_options_enabled);
    name_edit_->setEnabled(other_options_enabled);
    connections_spin_->setEnabled(other_options_enabled);
    referer_edit_->setEnabled(other_options_enabled);
    limit_editor_->setEnabled(task != nullptr && item->state != DownloadItem::State::Complete);

    if (!options_initialized_) {
        QString dir = item->output_dir;
        if (dir.isEmpty()) {
            dir = QString::fromStdString(settings_.download_dir);
            if (settings_.category_folders && !item->category.isEmpty()) {
                dir += QLatin1Char('/') + item->category;
            }
        }
        dir_edit_->setText(dir);
        name_edit_->setText(item->file_name);
        connections_spin_->setValue(task ? task->options().connections : 0);
        limit_editor_->set_value(item->bandwidth_limit_enabled,
                                 item->bandwidth_limit_kbps);
        referer_edit_->setText(task ? task->options().referer : QString());
        options_initialized_ = true;
    } else if (!limit_debounce_->isActive() &&
               !(task && task->active() && limit_editor_->hasFocus())) {
        limit_editor_->set_value(item->bandwidth_limit_enabled,
                                 item->bandwidth_limit_kbps);
    }
    refresh_connections();
}

void DownloadWindow::apply_live_limit() {
    const int row = model_->row_for_id(item_id_);
    DownloadItem* item = model_->item_at(row);
    if (!item || !item->task) return;
    if (item->task->active()) {
        emit limitEdited(item_id_, limit_editor_->enabled(), limit_editor_->kbps());
        return;
    }

    item->bandwidth_limit_enabled = limit_editor_->enabled();
    item->bandwidth_limit_kbps = limit_editor_->kbps();
    item->task->set_live_limit(item->bandwidth_limit_enabled,
                               item->bandwidth_limit_kbps);
    emit limitEdited(item_id_, item->bandwidth_limit_enabled,
                     item->bandwidth_limit_kbps);
}

void DownloadWindow::update_url_label() {
    if (url_text_.isEmpty()) {
        url_label_->clear();
        url_label_->setToolTip(QString());
        return;
    }
    const QFontMetrics metrics(url_label_->font());
    // Derived from the dialog width so the first elide already fits the default
    // window (the label itself has no meaningful width before the first show).
    const int available = std::max(kMinUrlLabelWidth, width() - kUrlLabelChrome);
    const QString display = metrics.elidedText(url_text_, Qt::ElideMiddle, available);
    url_label_->setText(QStringLiteral("<a href=\"%1\">%2</a>")
                            .arg(url_text_.toHtmlEscaped(), display.toHtmlEscaped()));
    url_label_->setToolTip(url_text_);
}

void DownloadWindow::resizeEvent(QResizeEvent* event) {
    QDialog::resizeEvent(event);
    update_url_label();
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
