#include "ui/properties_dialog.h"

#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFont>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>

#include "core/util.h"
#include "ui/download_task.h"
#include "ui/icons.h"

namespace swiftdm::ui {

namespace {

QString format_bytes_q(quint64 bytes) {
    return QString::fromStdString(swiftdm::format_bytes(bytes, 3));
}

}  // namespace

PropertiesDialog::PropertiesDialog(const DownloadItem& item, const DownloadTask* task,
                                   QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(tr("Download Properties"));
    setMinimumWidth(580);

    auto* layout = new QVBoxLayout(this);

    // Header: icon + file name.
    auto* header = new QHBoxLayout;
    auto* icon = new QLabel(this);
    icon->setPixmap(
        tinted_icon(QStringLiteral(":/icons/properties.svg"), colors::kActionBlue)
            .pixmap(QSize(28, 28)));
    header->addWidget(icon);
    auto* name = new QLabel(item.file_name.isEmpty() ? tr("Download") : item.file_name, this);
    QFont name_font = name->font();
    name_font.setBold(true);
    name->setFont(name_font);
    header->addWidget(name, 1);
    layout->addLayout(header);

    auto* form = new QFormLayout;

    form->addRow(tr("File Size :"),
                 new QLabel(item.size > 0
                                ? tr("%1 Bytes ( %2 )").arg(item.size).arg(format_bytes_q(item.size))
                                : tr("Unknown"),
                            this));
    auto* status = new QLabel(download_status_text(item), this);
    status->setStyleSheet(QStringLiteral("color:%1;").arg(download_status_color(item).name()));
    form->addRow(tr("Status :"), status);

    const QString dir =
        item.dest_path.isEmpty() ? item.output_dir : QFileInfo(item.dest_path).absolutePath();
    auto* saved = new QWidget(this);
    auto* saved_layout = new QHBoxLayout(saved);
    saved_layout->setContentsMargins(0, 0, 0, 0);
    auto* saved_edit = new QLineEdit(dir, saved);
    saved_edit->setReadOnly(true);
    saved_layout->addWidget(saved_edit, 1);
    auto* open_folder = new QPushButton(tr("Open Folder"), saved);
    open_folder->setEnabled(!dir.isEmpty() && QFileInfo(dir).isDir());
    saved_layout->addWidget(open_folder);
    connect(open_folder, &QPushButton::clicked, this,
            [dir] { QDesktopServices::openUrl(QUrl::fromLocalFile(dir)); });
    form->addRow(tr("Saved To :"), saved);

    auto* temp_edit = new QLineEdit(task ? task->workspace_dir() : QString(), this);
    temp_edit->setReadOnly(true);
    form->addRow(tr("Temp Path :"), temp_edit);

    auto* url_edit = new QLineEdit(item.url, this);
    url_edit->setReadOnly(true);
    form->addRow(tr("URL :"), url_edit);

    if (!item.page_url.isEmpty()) {
        auto* page_edit = new QLineEdit(item.page_url, this);
        page_edit->setReadOnly(true);
        form->addRow(tr("Page URL :"), page_edit);
    }
    if (!item.page_title.isEmpty()) {
        auto* page_title = new QLabel(item.page_title, this);
        page_title->setWordWrap(true);
        form->addRow(tr("Page Title :"), page_title);
    }

    form->addRow(tr("Category :"),
                 new QLabel(item.category.isEmpty() ? tr("Misc") : item.category, this));
    form->addRow(tr("Downloaded :"),
                 new QLabel(tr("%1 ( %2% )").arg(format_bytes_q(item.done)).arg(item.percent),
                            this));
    form->addRow(tr("Resumable :"), new QLabel(item.resumable ? tr("Yes") : tr("No"), this));

    const DownloadTask::Snapshot snap = task ? task->snapshot() : DownloadTask::Snapshot{};
    form->addRow(tr("Segments :"), new QLabel(QString::number(snap.segments.size()), this));

    form->addRow(tr("Added On :"), new QLabel(item.added, this));
    form->addRow(tr("Last Try :"), new QLabel(item.last_try, this));

    layout->addLayout(form);
    layout->addStretch(1);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    layout->addWidget(buttons);
}

}  // namespace swiftdm::ui
