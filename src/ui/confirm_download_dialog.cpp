#include "ui/confirm_download_dialog.h"

#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

#include "core/util.h"
#include "engine/categories.h"

namespace swiftdm::ui {

ConfirmDownloadDialog::ConfirmDownloadDialog(const QString& file_name, quint64 size,
                                             const QString& url,
                                             const QString& download_dir,
                                             bool category_folders, QWidget* parent)
    : QDialog(parent),
      download_dir_(download_dir),
      category_folders_(category_folders) {
    setWindowTitle(tr("Download Confirmation"));
    setMinimumWidth(600);

    auto* layout = new QVBoxLayout(this);
    layout->setSpacing(12);

    auto* grid = new QGridLayout;
    grid->setHorizontalSpacing(16);
    grid->setVerticalSpacing(8);

    const auto add_label = [this, grid](int row, const QString& text) {
        grid->addWidget(new QLabel(text, this), row, 0, Qt::AlignLeft | Qt::AlignTop);
    };

    // File: server-resolved name plus its size.
    add_label(0, tr("File :"));
    const QString shown_name =
        file_name.isEmpty() ? tr("(from URL)") : file_name;
    const QString size_text =
        size > 0 ? QString::fromStdString(swiftdm::format_bytes(size, 3))
                 : tr("Unknown size");
    auto* file_label = new QLabel(QStringLiteral("%1  (%2)").arg(shown_name, size_text), this);
    file_label->setWordWrap(true);
    file_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    file_label->setToolTip(QStringLiteral("%1 (%2)").arg(shown_name, size_text));
    grid->addWidget(file_label, 0, 1, Qt::AlignLeft);

    // URL: full value in a read-only field (selectable and scrollable).
    add_label(1, tr("URL :"));
    auto* url_edit = new QLineEdit(url, this);
    url_edit->setReadOnly(true);
    url_edit->setCursorPosition(0);
    url_edit->setToolTip(url);
    grid->addWidget(url_edit, 1, 1);

    // Category: extension-based default, overridable by the user.
    add_label(2, tr("Category :"));
    category_box_ = new QComboBox(this);
    for (const std::string& name : swiftdm::category_names()) {
        category_box_->addItem(QString::fromStdString(name));
    }
    const QString default_category =
        QString::fromStdString(swiftdm::category_folder_for(file_name.toStdString()));
    const int default_index = category_box_->findText(
        swiftdm::is_category_name(default_category.toStdString()) ? default_category
                                                                  : QStringLiteral("Misc"));
    if (default_index >= 0) category_box_->setCurrentIndex(default_index);
    category_box_->setToolTip(
        tr("Used for the category tree and for the default save location."));
    grid->addWidget(category_box_, 2, 1, Qt::AlignLeft);

    // Location: category folder by default, overridable with a custom folder.
    add_label(3, tr("Location :"));
    auto* location_row = new QHBoxLayout;
    location_edit_ = new QLineEdit(this);
    location_edit_->setClearButtonEnabled(true);
    location_edit_->setToolTip(tr("Folder the file is saved to"));
    location_row->addWidget(location_edit_, 1);
    auto* browse_button = new QPushButton(tr("Browse..."), this);
    location_row->addWidget(browse_button);
    auto* location_container = new QWidget(this);
    location_container->setLayout(location_row);
    grid->addWidget(location_container, 3, 1);
    grid->setColumnStretch(1, 1);
    layout->addLayout(grid);

    queue_hint_ = new QLabel(this);
    queue_hint_->setWordWrap(true);
    queue_hint_->setStyleSheet(QStringLiteral("color:#e0a85a;"));
    queue_hint_->setVisible(false);
    layout->addWidget(queue_hint_);
    layout->addSpacing(4);

    auto* buttons = new QHBoxLayout;
    buttons->addStretch(1);
    download_button_ = new QPushButton(tr("Download Now"), this);
    download_button_->setDefault(true);
    auto* queue_button = new QPushButton(tr("Add to queue"), this);
    queue_button->setToolTip(tr("Not available yet: queueing is not implemented."));
    auto* cancel_button = new QPushButton(tr("Cancel"), this);
    buttons->addWidget(download_button_);
    buttons->addSpacing(16);
    buttons->addWidget(queue_button);
    buttons->addSpacing(16);
    buttons->addWidget(cancel_button);
    buttons->addStretch(1);
    layout->addLayout(buttons);

    // Fill the default location only after the category box exists.
    use_default_location();

    connect(category_box_, &QComboBox::currentTextChanged, this, [this] {
        if (!location_custom_) use_default_location();
    });
    connect(location_edit_, &QLineEdit::textEdited, this, [this] {
        location_custom_ = true;
    });
    connect(location_edit_, &QLineEdit::textChanged, this, [this](const QString& text) {
        download_button_->setEnabled(!text.trimmed().isEmpty());
    });
    connect(browse_button, &QPushButton::clicked, this,
            &ConfirmDownloadDialog::choose_location);
    connect(download_button_, &QPushButton::clicked, this,
            &ConfirmDownloadDialog::confirm_download);
    connect(queue_button, &QPushButton::clicked, this, [this] {
        queue_hint_->setText(
            tr("Add to queue is not available yet: queueing is still to be implemented. "
               "Use Download Now or Cancel."));
        queue_hint_->setVisible(true);
    });
    connect(cancel_button, &QPushButton::clicked, this, &QDialog::reject);
}

QString ConfirmDownloadDialog::category() const {
    return category_box_->currentText();
}

QString ConfirmDownloadDialog::location() const {
    QString text = location_edit_->text().trimmed();
    if (text.isEmpty()) return {};
    text = QString::fromStdString(swiftdm::expand_tilde(text.toStdString()));
    return QDir::cleanPath(text);
}

QString ConfirmDownloadDialog::default_location() const {
    QString base = download_dir_.trimmed();
    if (base.isEmpty()) base = QDir::homePath();
    base = QString::fromStdString(swiftdm::expand_tilde(base.toStdString()));
    if (category_folders_) {
        return QDir::cleanPath(QDir(base).filePath(category_box_->currentText()));
    }
    return QDir::cleanPath(base);
}

void ConfirmDownloadDialog::use_default_location() {
    location_edit_->setText(default_location());
}

void ConfirmDownloadDialog::choose_location() {
    const QString start = location_edit_->text().trimmed().isEmpty()
                              ? default_location()
                              : location();
    const QString dir = QFileDialog::getExistingDirectory(this, tr("Select Download Folder"),
                                                          start);
    if (dir.isEmpty()) return;
    location_custom_ = true;
    location_edit_->setText(QDir::cleanPath(dir));
}

void ConfirmDownloadDialog::confirm_download() {
    const QString dir = location();
    if (dir.isEmpty()) {
        QMessageBox::warning(this, tr("Download Confirmation"),
                             tr("Choose a folder the file is saved to."));
        return;
    }
    if (!QDir(dir).exists() && !QDir().mkpath(dir)) {
        QMessageBox::warning(this, tr("Download Confirmation"),
                             tr("The folder cannot be created:\n%1").arg(dir));
        return;
    }
    accept();
}

}  // namespace swiftdm::ui
