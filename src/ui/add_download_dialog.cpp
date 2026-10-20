#include "ui/add_download_dialog.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QUrl>
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

    auto* url_hint = new QLabel(
        tr("Enter an HTTP, HTTPS, FTP or FTPS URL."), this);
    url_hint->setStyleSheet(QStringLiteral("color: #e0a050"));
    url_hint->hide();
    layout->addWidget(url_hint);

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
    connect(url_edit_, &QLineEdit::textChanged, this, [this, url_hint](const QString& text) {
        const QString value = text.trimmed();
        const QUrl parsed = QUrl::fromUserInput(value);
        const QString scheme = parsed.scheme().toLower();
        const bool supported_scheme = scheme == QStringLiteral("http") ||
                                      scheme == QStringLiteral("https") ||
                                      scheme == QStringLiteral("ftp") ||
                                      scheme == QStringLiteral("ftps");
        const bool valid = !value.isEmpty() && parsed.isValid() && supported_scheme &&
                           !parsed.host().isEmpty();
        download_button_->setEnabled(valid);
        url_hint->setVisible(!value.isEmpty() && !valid);
    });
    connect(url_edit_, &QLineEdit::returnPressed, this, [this] {
        if (download_button_->isEnabled()) accept();
    });

    url_edit_->setFocus();
}

QString AddDownloadDialog::url() const {
    return QUrl::fromUserInput(url_edit_->text().trimmed()).toString(QUrl::FullyEncoded);
}

}  // namespace swiftdm::ui
