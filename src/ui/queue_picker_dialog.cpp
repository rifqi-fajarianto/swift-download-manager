#include "ui/queue_picker_dialog.h"

#include <QAbstractItemView>
#include <QDialogButtonBox>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPushButton>
#include <QVBoxLayout>

namespace swiftdm::ui {

QueuePickerDialog::QueuePickerDialog(const QVector<QueueChoice>& queues, QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(tr("Choose Queue"));
    setMinimumSize(kMinWindowWidth, kMinWindowHeight);
    resize(kDefaultWindowWidth, kDefaultWindowHeight);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);

    auto* prompt = new QLabel(tr("Choose the queue this download is added to:"), this);
    prompt->setWordWrap(true);
    layout->addWidget(prompt);

    list_ = new QListWidget(this);
    list_->setObjectName(QStringLiteral("queue_picker_list"));
    list_->setSelectionMode(QAbstractItemView::SingleSelection);
    for (const QueueChoice& queue : queues) {
        auto* item = new QListWidgetItem(
            tr("%1 (%n item(s))", nullptr, queue.item_count).arg(queue.name), list_);
        item->setData(Qt::UserRole, queue.id);
    }
    layout->addWidget(list_, 1);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Cancel | QDialogButtonBox::Ok, this);
    auto* cancel_button = buttons->button(QDialogButtonBox::Cancel);
    cancel_button->setObjectName(QStringLiteral("queue_picker_cancel"));
    auto* ok_button = buttons->button(QDialogButtonBox::Ok);
    ok_button->setObjectName(QStringLiteral("queue_picker_ok"));
    ok_button->setText(tr("Add to queue"));
    ok_button->setDefault(true);
    // No queue is preselected: accepting requires a deliberate choice.
    ok_button->setEnabled(false);
    layout->addWidget(buttons);

    connect(list_, &QListWidget::itemSelectionChanged, this, [this, ok_button] {
        ok_button->setEnabled(selected_queue_id() > 0);
    });
    connect(list_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem*) {
        if (selected_queue_id() > 0) accept();
    });
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    list_->setFocus();
}

qint64 QueuePickerDialog::selected_queue_id() const {
    const QListWidgetItem* item = list_->currentItem();
    if (!item || !item->isSelected()) return 0;
    return item->data(Qt::UserRole).toLongLong();
}

}  // namespace swiftdm::ui
