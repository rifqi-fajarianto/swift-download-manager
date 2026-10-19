#include "ui/confirm_download_dialog.h"

#include <QFontMetrics>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSizePolicy>
#include <QVBoxLayout>

#include "core/util.h"

namespace swiftdm::ui {

ConfirmDownloadDialog::ConfirmDownloadDialog(const QString& file_name, quint64 size,
                                             const QString& url, QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(tr("Download"));

    auto* layout = new QVBoxLayout(this);
    layout->setSpacing(12);

    auto* grid = new QGridLayout;
    grid->setHorizontalSpacing(16);
    grid->setVerticalSpacing(6);

    const auto add_row = [&](int row, const QString& name, const QString& value) {
        grid->addWidget(new QLabel(name, this), row, 0, Qt::AlignLeft | Qt::AlignTop);
        auto* value_label = new QLabel(value, this);
        value_label->setWordWrap(true);
        value_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        grid->addWidget(value_label, row, 1, Qt::AlignLeft);
        return value_label;
    };
    QLabel* name_label = add_row(0, tr("File Name :"),
                                 file_name.isEmpty() ? tr("(from URL)") : file_name);
    name_label->setWordWrap(false);
    name_label->setToolTip(file_name);
    if (file_name.size() > 64) {
        name_label->setText(
            QFontMetrics(name_label->font()).elidedText(file_name, Qt::ElideMiddle, 420));
    }
    add_row(1, tr("Size :"),
            size > 0 ? QString::fromStdString(swiftdm::format_bytes(size, 3))
                     : tr("Unknown"));
    // A URL without spaces cannot wrap, so an unbroken long URL would keep
    // widening the dialog. Elide it and keep the full text in the tooltip.
    QLabel* url_label = add_row(2, tr("URL :"), url);
    url_label->setWordWrap(false);
    url_label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    url_label->setText(QFontMetrics(url_label->font())
                           .elidedText(url, Qt::ElideMiddle, 420));
    url_label->setToolTip(url);
    grid->setColumnStretch(1, 1);
    layout->addLayout(grid);
    layout->addSpacing(4);

    auto* buttons = new QHBoxLayout;
    buttons->addStretch(1);
    auto* download_button = new QPushButton(tr("Download"), this);
    download_button->setDefault(true);
    auto* cancel_button = new QPushButton(tr("Cancel"), this);
    buttons->addWidget(download_button);
    buttons->addSpacing(28);
    buttons->addWidget(cancel_button);
    buttons->addStretch(1);
    layout->addLayout(buttons);

    connect(download_button, &QPushButton::clicked, this, &QDialog::accept);
    connect(cancel_button, &QPushButton::clicked, this, &QDialog::reject);

    setMinimumWidth(440);
}

}  // namespace swiftdm::ui
