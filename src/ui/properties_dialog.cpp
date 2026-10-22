#include "ui/properties_dialog.h"

#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFont>
#include <QFontMetrics>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QResizeEvent>
#include <QUrl>
#include <QVBoxLayout>

#include "core/util.h"
#include "ui/download_task.h"
#include "ui/icons.h"
#include "ui/section_box.h"

namespace swiftdm::ui {

namespace {

// Space taken by the window margins, the header icon and the header spacing.
// The file name gets what is left of the window width.
constexpr int kNameChrome = 2 * 12 + 28 + 8;
constexpr int kMinNameWidth = 160;

QLabel* add_value_row(QFormLayout* form, const QString& label, const QString& value,
                      const char* object_name) {
    auto* widget = new QLabel(value, form->parentWidget());
    widget->setObjectName(QString::fromLatin1(object_name));
    widget->setTextInteractionFlags(Qt::TextSelectableByMouse);
    widget->setWordWrap(false);
    form->addRow(label, widget);
    return widget;
}

QLineEdit* add_read_only_row(QFormLayout* form, const QString& label,
                             const QString& value, const char* object_name) {
    auto* edit = new QLineEdit(value, form->parentWidget());
    edit->setObjectName(QString::fromLatin1(object_name));
    edit->setReadOnly(true);
    edit->setCursorPosition(0);
    if (!value.isEmpty()) edit->setToolTip(value);
    form->addRow(label, edit);
    return edit;
}

QString format_bytes_q(quint64 bytes) {
    return QString::fromStdString(swiftdm::format_bytes(bytes, 3));
}

}  // namespace

PropertiesDialog::PropertiesDialog(const DownloadItem& item, const DownloadTask* task,
                                   QWidget* parent)
    : QDialog(parent),
      file_name_(item.file_name.isEmpty() ? tr("Download") : item.file_name) {
    setWindowTitle(tr("Download Properties"));
    // The sections provide the minimum size; resizing here only sets the
    // default. Do not force a minimum that could compress the rows.
    resize(650, 690);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(kSectionSpacing);

    // Header: icon + file name.
    auto* header = new QHBoxLayout;
    header->setSpacing(8);
    auto* icon = new QLabel(this);
    icon->setPixmap(
        tinted_icon(QStringLiteral(":/icons/properties.svg"), colors::kActionBlue)
            .pixmap(QSize(28, 28)));
    icon->setFixedSize(28, 28);
    header->addWidget(icon);

    name_label_ = new QLabel(this);
    name_label_->setObjectName(QStringLiteral("properties_file_name"));
    name_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    name_label_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    name_label_->setMinimumWidth(0);
    name_label_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    name_label_->setToolTip(file_name_);
    QFont name_font = name_label_->font();
    name_font.setBold(true);
    name_label_->setFont(name_font);
    header->addWidget(name_label_, 1);
    layout->addLayout(header);
    update_name_label();

    auto* general = add_form_section(layout, this, tr("General"));
    add_value_row(general, tr("File size"),
                  item.size > 0
                      ? tr("%1 Bytes ( %2 )").arg(item.size).arg(format_bytes_q(item.size))
                      : tr("Unknown"),
                  "properties_file_size");
    QLabel* status = add_value_row(general, tr("Status"), download_status_text(item),
                                   "properties_status");
    status->setStyleSheet(QStringLiteral("color:%1;").arg(download_status_color(item).name()));
    add_value_row(general, tr("Category"),
                  item.category.isEmpty() ? tr("Misc") : item.category,
                  "properties_category");
    add_value_row(general, tr("Downloaded"),
                  tr("%1 ( %2% )").arg(format_bytes_q(item.done)).arg(item.percent),
                  "properties_downloaded");
    add_value_row(general, tr("Resumable"), item.resumable ? tr("Yes") : tr("No"),
                  "properties_resumable");
    const DownloadTask::Snapshot snap = task ? task->snapshot() : DownloadTask::Snapshot{};
    add_value_row(general, tr("Segments"), QString::number(snap.segments.size()),
                  "properties_segments");

    auto* location = add_form_section(layout, this, tr("Location"));
    const QString dir =
        item.dest_path.isEmpty() ? item.output_dir : QFileInfo(item.dest_path).absolutePath();
    auto* saved = new QWidget(location->parentWidget());
    auto* saved_layout = new QHBoxLayout(saved);
    saved_layout->setContentsMargins(0, 0, 0, 0);
    saved_layout->setSpacing(6);
    auto* saved_edit = new QLineEdit(dir, saved);
    saved_edit->setObjectName(QStringLiteral("properties_saved_to"));
    saved_edit->setReadOnly(true);
    saved_edit->setCursorPosition(0);
    if (!dir.isEmpty()) saved_edit->setToolTip(dir);
    saved_layout->addWidget(saved_edit, 1);
    auto* open_folder = new QPushButton(tr("Open Folder"), saved);
    open_folder->setObjectName(QStringLiteral("properties_open_folder"));
    open_folder->setEnabled(!dir.isEmpty() && QFileInfo(dir).isDir());
    saved_layout->addWidget(open_folder);
    connect(open_folder, &QPushButton::clicked, this,
            [dir] { QDesktopServices::openUrl(QUrl::fromLocalFile(dir)); });
    location->addRow(tr("Saved to"), saved);

    const QString temp_path =
        task && !task->workspace_dir().isEmpty() ? task->workspace_dir() : item.temp_path;
    add_read_only_row(location, tr("Temp path"), temp_path, "properties_temp_path");

    auto* source = add_form_section(layout, this, tr("Source"));
    add_read_only_row(source, tr("URL"), item.url, "properties_url");
    if (!item.page_url.isEmpty()) {
        add_read_only_row(source, tr("Page URL"), item.page_url, "properties_page_url");
    }
    if (!item.page_title.isEmpty()) {
        QLabel* page_title =
            add_value_row(source, tr("Page title"), item.page_title, "properties_page_title");
        page_title->setWordWrap(true);
        // The wrapped title must use the width the form gives it instead of
        // asking the dialog to grow for a long page title.
        page_title->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        page_title->setMinimumWidth(0);
    }
    if (!item.group.isEmpty()) {
        add_read_only_row(source, tr("Pair"), item.group, "properties_pair");
    }

    auto* time = add_form_section(layout, this, tr("Time"));
    add_value_row(time, tr("Added on"), item.added, "properties_added_on");
    add_value_row(time, tr("Last try"), item.last_try, "properties_last_try");

    layout->addStretch(1);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    layout->addWidget(buttons);
}

void PropertiesDialog::resizeEvent(QResizeEvent* event) {
    QDialog::resizeEvent(event);
    update_name_label();
}

void PropertiesDialog::update_name_label() {
    if (!name_label_) return;
    const int available = qMax(kMinNameWidth, width() - kNameChrome);
    const QFontMetrics metrics(name_label_->font());
    name_label_->setText(metrics.elidedText(file_name_, Qt::ElideMiddle, available));
}

}  // namespace swiftdm::ui
