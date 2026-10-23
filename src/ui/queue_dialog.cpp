#include "ui/queue_dialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QSpinBox>
#include <QVBoxLayout>

#include "ui/section_box.h"
#include "ui/speed_limit_editor.h"

namespace swiftdm::ui {

QueuePropertiesDialog::QueuePropertiesDialog(
    const swiftdm::HistoryStore::QueueRecord& queue, int item_count, QWidget* parent)
    : QDialog(parent), queue_(queue) {
    setWindowTitle(tr("Queue Properties"));
    resize(kDefaultWindowWidth, kDefaultWindowHeight);

    auto* page = new QVBoxLayout(this);
    page->setContentsMargins(12, 12, 12, 12);
    page->setSpacing(kSectionSpacing);

    auto* general = add_form_section(page, this, tr("Queue"));
    name_ = new QLineEdit(QString::fromStdString(queue_.name), this);
    name_->setObjectName(QStringLiteral("queue_name"));
    general->addRow(tr("Name:"), name_);

    auto* items = new QLabel(tr("%n item(s)", nullptr, item_count), this);
    items->setObjectName(QStringLiteral("queue_item_count"));
    general->addRow(tr("Items:"), items);

    max_simultaneous_ = new QSpinBox(this);
    max_simultaneous_->setObjectName(QStringLiteral("queue_max"));
    max_simultaneous_->setRange(0, 64);
    max_simultaneous_->setSpecialValueText(tr("Unlimited"));
    max_simultaneous_->setValue(queue_.max_simultaneous);
    max_simultaneous_->setToolTip(
        tr("0 = Unlimited: start every item in this queue at once."));
    general->addRow(tr("Simultaneous downloads:"), max_simultaneous_);

    connections_ = new QSpinBox(this);
    connections_->setObjectName(QStringLiteral("queue_connections"));
    connections_->setRange(0, 64);
    connections_->setSpecialValueText(tr("Default"));
    connections_->setValue(queue_.connections);
    general->addRow(tr("Connections per download:"), connections_);

    auto* bandwidth = add_section(page, this, tr("Bandwidth"));
    limit_ = new SpeedLimitEditor(this);
    limit_->setObjectName(QStringLiteral("queue_limit"));
    limit_->set_value(queue_.bandwidth_limit_enabled, queue_.bandwidth_limit_kbps);
    bandwidth->addWidget(limit_);
    auto* bandwidth_note =
        new QLabel(tr("The limit applies to the whole queue and is shared by its "
                      "active downloads."),
                   this);
    bandwidth_note->setWordWrap(true);
    bandwidth_note->setStyleSheet(QStringLiteral("color:#a0a0a0;"));
    bandwidth->addWidget(bandwidth_note);

    auto* behavior = new QVBoxLayout;
    auto* behavior_box = add_section(page, this, tr("Behavior"));
    autostart_ = new QCheckBox(tr("Start this queue automatically when the application starts"),
                               this);
    autostart_->setObjectName(QStringLiteral("queue_autostart"));
    autostart_->setChecked(queue_.autostart);
    behavior->addWidget(autostart_);

    notify_item_ = new QCheckBox(tr("Notify when an item completes"), this);
    notify_item_->setObjectName(QStringLiteral("queue_notify_item"));
    notify_item_->setChecked(queue_.notify_item);
    behavior->addWidget(notify_item_);

    notify_complete_ = new QCheckBox(tr("Notify when all items complete"), this);
    notify_complete_->setObjectName(QStringLiteral("queue_notify_complete"));
    notify_complete_->setChecked(queue_.notify_complete);
    behavior->addWidget(notify_complete_);

    auto* shutdown_row = new QHBoxLayout;
    shutdown_ = new QCheckBox(tr("Shut down the computer when all items complete"), this);
    shutdown_->setObjectName(QStringLiteral("queue_shutdown"));
    shutdown_->setChecked(queue_.shutdown_when_complete);
    shutdown_delay_ = new QSpinBox(this);
    shutdown_delay_->setObjectName(QStringLiteral("queue_shutdown_delay"));
    shutdown_delay_->setRange(5, 3600);
    shutdown_delay_->setSuffix(tr(" s"));
    shutdown_delay_->setValue(queue_.shutdown_delay > 0 ? queue_.shutdown_delay : 60);
    shutdown_delay_->setEnabled(shutdown_->isChecked());
    connect(shutdown_, &QCheckBox::toggled, shutdown_delay_, &QWidget::setEnabled);
    shutdown_row->addWidget(shutdown_);
    shutdown_row->addWidget(shutdown_delay_);
    shutdown_row->addStretch(1);
    behavior->addLayout(shutdown_row);
    behavior_box->addLayout(behavior);

    auto* shutdown_note =
        new QLabel(tr("Other active downloads and queues are paused before shutdown. "
                      "The countdown can be cancelled."),
                   this);
    shutdown_note->setWordWrap(true);
    shutdown_note->setStyleSheet(QStringLiteral("color:#a0a0a0;"));
    behavior_box->addWidget(shutdown_note);

    page->addStretch(1);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    page->addWidget(buttons);
}

swiftdm::HistoryStore::QueueRecord QueuePropertiesDialog::result() const {
    swiftdm::HistoryStore::QueueRecord queue = queue_;
    queue.name = name_->text().trimmed().toStdString();
    if (queue.name.empty()) queue.name = "Queue";
    queue.max_simultaneous = max_simultaneous_->value();
    queue.connections = connections_->value();
    queue.bandwidth_limit_enabled = limit_->enabled();
    queue.bandwidth_limit_kbps = limit_->kbps();
    queue.autostart = autostart_->isChecked();
    queue.notify_item = notify_item_->isChecked();
    queue.notify_complete = notify_complete_->isChecked();
    queue.shutdown_when_complete = shutdown_->isChecked();
    queue.shutdown_delay = shutdown_delay_->value();
    return queue;
}

}  // namespace swiftdm::ui
