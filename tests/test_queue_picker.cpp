// Headless check of the queue picker dialog: the queue entries with their item
// counts, the geometry and the deliberate selection rule. The dialog is never
// shown, so this stays within the CLI/backend-only test policy (AGENTS.md §6).

#include <QApplication>
#include <QListWidget>
#include <QPushButton>
#include <QSize>

#include <cstdio>

#include "ui/queue_picker_dialog.h"

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

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);

    using swiftdm::ui::QueuePickerDialog;

    QueuePickerDialog picker({{7, QStringLiteral("Batch"), 2},
                              {9, QStringLiteral("Parts"), 0}});
    auto* list = named<QListWidget>(&picker, "queue_picker_list");
    auto* ok = named<QPushButton>(&picker, "queue_picker_ok");
    auto* cancel = named<QPushButton>(&picker, "queue_picker_cancel");

    check(picker.windowTitle() == QStringLiteral("Choose Queue"),
          "the window title is Choose Queue");
    check(picker.minimumSize() == QSize(QueuePickerDialog::kMinWindowWidth,
                                        QueuePickerDialog::kMinWindowHeight),
          "the picker declares a fixed minimum size");
    check(picker.size() == QSize(QueuePickerDialog::kDefaultWindowWidth,
                                 QueuePickerDialog::kDefaultWindowHeight),
          "the picker opens with the default size");
    check(list && list->count() == 2, "the picker lists every queue");
    check(list && list->item(0)->text().contains(QStringLiteral("Batch")) &&
              list->item(0)->text().contains(QStringLiteral("2")),
          "the first queue shows its name and item count");
    check(list && list->item(1)->text().contains(QStringLiteral("Parts")),
          "the second queue shows its name");
    check(ok && ok->text() == QStringLiteral("Add to queue"),
          "the accept button says Add to queue");
    check(cancel && cancel->text() == QStringLiteral("Cancel"),
          "the cancel button says Cancel");
    check(picker.selected_queue_id() == 0,
          "nothing is selected when the picker opens");
    check(ok && !ok->isEnabled(),
          "accepting needs a deliberate choice");

    if (list) list->setCurrentRow(0);
    check(picker.selected_queue_id() == 7, "selecting a queue reports its id");
    check(ok && ok->isEnabled(), "accepting is enabled after a choice");
    if (ok) ok->click();
    check(picker.result() == QDialog::Accepted, "OK accepts the picker");
    check(picker.selected_queue_id() == 7, "the chosen queue survives acceptance");

    QueuePickerDialog double_click({{7, QStringLiteral("Batch"), 2},
                                    {9, QStringLiteral("Parts"), 0}});
    auto* double_list = named<QListWidget>(&double_click, "queue_picker_list");
    if (double_list) {
        double_list->setCurrentRow(1);
        double_list->itemDoubleClicked(double_list->item(1));
    }
    check(double_click.result() == QDialog::Accepted,
          "a double click accepts the picker");
    check(double_click.selected_queue_id() == 9,
          "the double-clicked queue is the target");

    QueuePickerDialog canceled({{7, QStringLiteral("Batch"), 2}});
    auto* canceled_cancel = named<QPushButton>(&canceled, "queue_picker_cancel");
    if (canceled_cancel) canceled_cancel->click();
    check(canceled.result() == QDialog::Rejected, "Cancel rejects the picker");
    check(canceled.selected_queue_id() == 0,
          "a canceled picker reports no queue");

    if (failures == 0) std::printf("queue_picker: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
