#include "ui/confirm_download_dialog.h"

#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
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
    };
    add_row(0, tr("File Name :"), file_name.isEmpty() ? tr("(from URL)") : file_name);
    add_row(1, tr("Size :"),
            size > 0 ? QString::fromStdString(swiftdm::format_bytes(size, 3))
                     : tr("Unknown"));
    add_row(2, tr("URL :"), url);
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
