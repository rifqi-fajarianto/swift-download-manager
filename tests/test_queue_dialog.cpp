// Headless check of the Queue Properties dialog: the sections, every field and
// the round-trip through result(). The dialog is never shown, so this stays
// within the CLI/backend-only test policy (AGENTS.md §6).

#include <QApplication>
#include <QCheckBox>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QSpinBox>
#include <QStringList>

#include <cstdio>

#include "ui/queue_dialog.h"
#include "ui/speed_limit_editor.h"

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

template <typename T>
T* named(QWidget* root, const char* name) {
    return root->findChild<T*>(QString::fromLatin1(name));
}

QStringList section_titles(const QWidget* root) {
    QStringList titles;
    for (const QGroupBox* box : root->findChildren<QGroupBox*>()) {
        titles << box->title();
    }
    return titles;
}

const QGroupBox* section_with_title(const QWidget* root, const QString& title) {
    for (const QGroupBox* box : root->findChildren<QGroupBox*>()) {
        if (box->title() == title) return box;
    }
    return nullptr;
}

bool is_in_section(const QWidget* root, const QString& title, const QWidget* widget) {
    const QGroupBox* section = section_with_title(root, title);
    return section && widget && section->isAncestorOf(widget);
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);

    using swiftdm::HistoryStore;
    using swiftdm::ui::QueuePropertiesDialog;
    using swiftdm::ui::SpeedLimitEditor;

    HistoryStore::QueueRecord queue;
    queue.id = 42;
    queue.name = "Batch";
    queue.max_simultaneous = 3;
    queue.connections = 4;
    queue.bandwidth_limit_enabled = true;
    queue.bandwidth_limit_kbps = 2048;
    queue.autostart = true;
    queue.notify_item = true;
    queue.notify_complete = false;
    queue.shutdown_when_complete = true;
    queue.shutdown_delay = 30;

    QueuePropertiesDialog dialog(queue, 5);
    check(dialog.windowTitle() == QStringLiteral("Queue Properties"),
          "the window title is Queue Properties");
    check(dialog.size() == QSize(QueuePropertiesDialog::kDefaultWindowWidth,
                                 QueuePropertiesDialog::kDefaultWindowHeight),
          "the dialog opens with the default size");
    check(section_titles(&dialog) ==
              QStringList({QStringLiteral("Queue"), QStringLiteral("Bandwidth"),
                           QStringLiteral("Behavior")}),
          "the dialog has Queue, Bandwidth and Behavior sections in order");

    auto* name = named<QLineEdit>(&dialog, "queue_name");
    auto* item_count = named<QLabel>(&dialog, "queue_item_count");
    auto* max_simultaneous = named<QSpinBox>(&dialog, "queue_max");
    auto* connections = named<QSpinBox>(&dialog, "queue_connections");
    auto* limit = named<SpeedLimitEditor>(&dialog, "queue_limit");
    auto* autostart = named<QCheckBox>(&dialog, "queue_autostart");
    auto* notify_item = named<QCheckBox>(&dialog, "queue_notify_item");
    auto* notify_complete = named<QCheckBox>(&dialog, "queue_notify_complete");
    auto* shutdown = named<QCheckBox>(&dialog, "queue_shutdown");
    auto* shutdown_delay = named<QSpinBox>(&dialog, "queue_shutdown_delay");

    check(name && name->text() == QStringLiteral("Batch"),
          "the queue name is loaded");
    check(item_count && item_count->text().contains(QStringLiteral("5")),
          "the item count is shown");
    check(max_simultaneous && max_simultaneous->value() == 3 &&
              max_simultaneous->minimum() == 0 && max_simultaneous->maximum() == 64,
          "the simultaneous limit is loaded with the 0-64 range");
    check(max_simultaneous && max_simultaneous->specialValueText() ==
                                   QStringLiteral("Unlimited"),
          "zero is shown as Unlimited");
    check(connections && connections->value() == 4 &&
              connections->specialValueText() == QStringLiteral("Default"),
          "zero connections means the global default");
    check(limit && limit->enabled() && limit->kbps() == 2048,
          "the queue speed limit is loaded");
    check(autostart && autostart->isChecked(), "autostart is loaded");
    check(notify_item && notify_item->isChecked(),
          "the per-item notification option is loaded");
    check(notify_complete && !notify_complete->isChecked(),
          "the all-complete notification option is loaded");
    check(shutdown && shutdown->isChecked() && shutdown_delay &&
              shutdown_delay->value() == 30 && shutdown_delay->isEnabled(),
          "the shutdown option and delay are loaded");
    check(shutdown_delay && shutdown_delay->minimum() == 5,
          "the shutdown delay has a five-second floor");

    check(is_in_section(&dialog, QStringLiteral("Queue"), name) &&
              is_in_section(&dialog, QStringLiteral("Queue"), item_count) &&
              is_in_section(&dialog, QStringLiteral("Queue"), max_simultaneous) &&
              is_in_section(&dialog, QStringLiteral("Queue"), connections),
          "the name and concurrency fields sit in the Queue section");
    check(is_in_section(&dialog, QStringLiteral("Bandwidth"), limit),
          "the speed limit sits in the Bandwidth section");
    check(is_in_section(&dialog, QStringLiteral("Behavior"), autostart) &&
              is_in_section(&dialog, QStringLiteral("Behavior"), notify_item) &&
              is_in_section(&dialog, QStringLiteral("Behavior"), notify_complete) &&
              is_in_section(&dialog, QStringLiteral("Behavior"), shutdown) &&
              is_in_section(&dialog, QStringLiteral("Behavior"), shutdown_delay),
          "the behavior options sit in the Behavior section");

    // The delay only makes sense while the shutdown option is on.
    shutdown->setChecked(false);
    check(shutdown_delay && !shutdown_delay->isEnabled(),
          "the shutdown delay is disabled with the option off");
    shutdown->setChecked(true);
    check(shutdown_delay && shutdown_delay->isEnabled(),
          "the shutdown delay is enabled with the option on");

    // Zero displays its special text in both spin boxes.
    max_simultaneous->setValue(0);
    connections->setValue(0);
    check(max_simultaneous->text() == QStringLiteral("Unlimited"),
          "a zero simultaneous limit displays Unlimited");
    check(connections->text() == QStringLiteral("Default"),
          "zero connections displays Default");

    name->setText(QStringLiteral("Renamed"));
    connections->setValue(2);
    limit->set_value(false, 512);
    autostart->setChecked(false);
    notify_item->setChecked(false);
    notify_complete->setChecked(true);
    shutdown->setChecked(false);
    shutdown_delay->setValue(45);

    const HistoryStore::QueueRecord updated = dialog.result();
    check(updated.id == 42 && updated.name == "Renamed",
          "result() preserves the id and takes the new name");
    check(updated.max_simultaneous == 0 && updated.connections == 2,
          "result() takes the concurrency settings");
    check(!updated.bandwidth_limit_enabled && updated.bandwidth_limit_kbps == 512,
          "result() takes the disabled limiter and its kept value");
    check(!updated.autostart && !updated.notify_item && updated.notify_complete,
          "result() takes the notification toggles");
    check(!updated.shutdown_when_complete && updated.shutdown_delay == 45,
          "result() takes the shutdown option and delay");

    name->setText(QStringLiteral("   "));
    check(dialog.result().name == "Queue",
          "an empty name falls back to Queue");

    if (failures == 0) std::printf("queue_dialog: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
