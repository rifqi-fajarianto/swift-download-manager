#include "ui/refresh_waiting_dialog.h"

#include <QCloseEvent>
#include <QFont>
#include <QFontMetrics>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include "core/util.h"
#include "ui/icons.h"

namespace swiftdm::ui {

namespace {

// Muted color for the field labels; the values use the normal text color.
const char* kFieldLabelStyle = "color: #9aa0a6;";

}  // namespace

RefreshWaitingDialog::RefreshWaitingDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Refresh Download URL"));
    // Floating helper window: no modal loop, stays above the browser, and does
    // not take a taskbar slot.
    setWindowFlags(Qt::Dialog | Qt::WindowStaysOnTopHint | Qt::WindowTitleHint |
                   Qt::CustomizeWindowHint | Qt::WindowCloseButtonHint);
    setModal(false);
    // Compact by default; the form is two short rows so the window stays small.
    setFixedWidth(340);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 10, 12, 10);
    layout->setSpacing(8);

    // Headline row: refresh glyph + one-line status.
    auto* head = new QHBoxLayout;
    head->setSpacing(8);
    auto* icon = new QLabel(this);
    icon->setPixmap(tinted_icon(QStringLiteral(":/icons/refresh-download.svg"),
                                colors::kActionBlue)
                        .pixmap(18, 18));
    icon->setFixedSize(18, 18);
    head->addWidget(icon, 0, Qt::AlignVCenter);

    auto* headline = new QLabel(tr("Waiting for a new download URL\u2026"), this);
    QFont head_font = headline->font();
    head_font.setBold(true);
    headline->setFont(head_font);
    headline->setWordWrap(true);
    head->addWidget(headline, 1);
    layout->addLayout(head);

    // Two compact label/value rows.
    auto* form = new QGridLayout;
    form->setContentsMargins(0, 0, 0, 0);
    form->setHorizontalSpacing(8);
    form->setVerticalSpacing(2);
    form->setColumnStretch(1, 1);

    auto* name_label = new QLabel(tr("File Name:"), this);
    name_label->setStyleSheet(QLatin1String(kFieldLabelStyle));
    form->addWidget(name_label, 0, 0, Qt::AlignLeft | Qt::AlignTop);

    name_value_ = new QLabel(this);
    name_value_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addWidget(name_value_, 0, 1);

    auto* size_label = new QLabel(tr("File Size:"), this);
    size_label->setStyleSheet(QLatin1String(kFieldLabelStyle));
    form->addWidget(size_label, 1, 0, Qt::AlignLeft);

    size_value_ = new QLabel(this);
    size_value_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addWidget(size_value_, 1, 1);

    layout->addLayout(form);

    auto* hint = new QLabel(
        tr("Start this file again in your browser. SwiftDM will ask you to "
           "confirm the new URL."),
        this);
    hint->setWordWrap(true);
    hint->setStyleSheet(QLatin1String(kFieldLabelStyle));
    layout->addWidget(hint);

    auto* buttons = new QHBoxLayout;
    buttons->setContentsMargins(0, 0, 0, 0);
    buttons->addStretch(1);
    cancel_button_ = new QPushButton(tr("Cancel"), this);
    buttons->addWidget(cancel_button_);
    layout->addLayout(buttons);

    connect(cancel_button_, &QPushButton::clicked, this, [this] {
        hide();
        emit cancelRequested();
    });
}

void RefreshWaitingDialog::set_file_info(const QString& name, quint64 size) {
    file_name_ = name;
    update_file_name();
    if (size > 0) {
        size_value_->setText(
            QString::fromStdString(swiftdm::format_bytes(size, 2)));
    } else {
        size_value_->setText(tr("unknown"));
    }
}

void RefreshWaitingDialog::update_file_name() {
    if (!name_value_) return;
    // Elide in the middle so a very long name cannot widen the fixed-width
    // window; the full name stays available as a tooltip.
    const int available = qMax(80, width() - 140);
    const QFontMetrics metrics(name_value_->font());
    name_value_->setText(metrics.elidedText(file_name_, Qt::ElideMiddle, available));
    name_value_->setToolTip(file_name_);
}

void RefreshWaitingDialog::dismiss() {
    dismissing_ = true;
    hide();
    dismissing_ = false;
}

void RefreshWaitingDialog::closeEvent(QCloseEvent* event) {
    if (dismissing_) {
        event->accept();
        return;
    }
    // The title-bar close button cancels the wait, same as Cancel.
    hide();
    event->ignore();
    emit cancelRequested();
}

}  // namespace swiftdm::ui
