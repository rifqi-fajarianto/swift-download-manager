#include "ui/add_download_dialog.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace swiftdm::ui {

AddDownloadDialog::AddDownloadDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("New URL"));

    auto* layout = new QVBoxLayout(this);
    layout->setSpacing(12);

    auto* row = new QHBoxLayout;
    row->addWidget(new QLabel(tr("URL :"), this));
    url_edit_ = new QLineEdit(this);
    url_edit_->setMinimumWidth(420);
    url_edit_->setClearButtonEnabled(true);
    url_edit_->setPlaceholderText(tr("https://example.com/file.zip"));
    row->addWidget(url_edit_, 1);
    layout->addLayout(row);

    auto* buttons = new QHBoxLayout;
    buttons->addStretch(1);
    download_button_ = new QPushButton(tr("Download"), this);
    download_button_->setDefault(true);
    download_button_->setEnabled(false);
    auto* cancel_button = new QPushButton(tr("Cancel"), this);
    buttons->addWidget(download_button_);
    buttons->addSpacing(28);
    buttons->addWidget(cancel_button);
    buttons->addStretch(1);
    layout->addLayout(buttons);

    connect(cancel_button, &QPushButton::clicked, this, &QDialog::reject);
    connect(download_button_, &QPushButton::clicked, this, &QDialog::accept);
    connect(url_edit_, &QLineEdit::textChanged, this, [this](const QString& text) {
        download_button_->setEnabled(!text.trimmed().isEmpty());
    });
    connect(url_edit_, &QLineEdit::returnPressed, this, [this] {
        if (download_button_->isEnabled()) accept();
    });

    url_edit_->setFocus();
}

QString AddDownloadDialog::url() const {
    return url_edit_->text().trimmed();
}

}  // namespace swiftdm::ui
