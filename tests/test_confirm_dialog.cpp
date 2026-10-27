// Headless check of the download confirmation dialog: the File/Save sections,
// the separate file size row, the fixed geometry and the button row. The
// dialog is never shown, so this stays within the CLI/backend-only test policy
// (AGENTS.md §6).

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QGroupBox>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QList>
#include <QPushButton>
#include <QStringList>

#include <cmath>
#include <cstdio>

#include "core/util.h"
#include "ui/confirm_download_dialog.h"

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

QStringList label_texts(const QWidget* widget) {
    QStringList texts;
    for (const QLabel* label : widget->findChildren<QLabel*>()) texts << label->text();
    return texts;
}

QStringList section_titles(const QWidget* root) {
    QStringList titles;
    for (const QGroupBox* box : root->findChildren<QGroupBox*>()) titles << box->title();
    return titles;
}

const QGroupBox* section_with_title(const QWidget* root, const QString& title) {
    for (const QGroupBox* box : root->findChildren<QGroupBox*>()) {
        if (box->title() == title) return box;
    }
    return nullptr;
}

QWidget* named_widget(const QWidget* root, const char* name) {
    return root->findChild<QWidget*>(QString::fromLatin1(name));
}

bool is_in_section(const QWidget* root, const QString& title, const char* name) {
    const QGroupBox* section = section_with_title(root, title);
    const QWidget* widget = named_widget(root, name);
    return section && widget && section->isAncestorOf(widget);
}

const QLabel* label_with_text(const QWidget* root, const QString& text) {
    for (const QLabel* label : root->findChildren<QLabel*>()) {
        if (label->text() == text) return label;
    }
    return nullptr;
}

// Vertical center of a widget, in the coordinates of the dialog. Widgets
// inside a container (the location field) are mapped as well.
double center_y(const QWidget* widget, const QWidget* root) {
    const QPoint top = widget->mapTo(root, QPoint(0, 0));
    return top.y() + widget->height() / 2.0;
}

// The dialog is never shown, so the nested layouts (the location row) are
// activated by hand: without them the child widgets keep their default
// geometry and the row measurements are meaningless.
void activate_layouts(QWidget* widget) {
    QLayout* layout = widget->layout();
    if (!layout) return;
    // activate() alone does not assign child geometry while the dialog is
    // hidden, so give the layout an explicit rectangle first.
    layout->setGeometry(widget->contentsRect());
    layout->activate();
    for (int i = 0; i < layout->count(); ++i) {
        QLayoutItem* item = layout->itemAt(i);
        if (!item) continue;
        if (QWidget* child = item->widget()) {
            activate_layouts(child);
            continue;
        }
        QLayout* nested = item->layout();
        if (!nested) continue;
        nested->activate();
        for (int j = 0; j < nested->count(); ++j) {
            QLayoutItem* nested_item = nested->itemAt(j);
            if (!nested_item) continue;
            if (QWidget* child = nested_item->widget()) activate_layouts(child);
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);

    using swiftdm::ui::ConfirmDownloadDialog;

    const QString long_name = QString(400, QLatin1Char('n')) + QStringLiteral(".mp4");
    const QString long_url =
        QStringLiteral("https://example.com/") + QString(400, QLatin1Char('p'));

    ConfirmDownloadDialog dialog(QStringLiteral("movie.mp4"), 1500000,
                                 QStringLiteral("https://example.com/movie.mp4"),
                                 QStringLiteral("/tmp"), true);

    check(dialog.windowTitle() == QStringLiteral("Download Confirmation"),
          "the window title is Download Confirmation");
    check(dialog.minimumSize() == QSize(ConfirmDownloadDialog::kMinWindowWidth,
                                        ConfirmDownloadDialog::kMinWindowHeight),
          "the dialog declares a fixed minimum size");
    check(dialog.size() == QSize(ConfirmDownloadDialog::kDefaultWindowWidth,
                                 ConfirmDownloadDialog::kDefaultWindowHeight),
          "the dialog opens with the default size");
    check(dialog.layout()->sizeHint().height() <= ConfirmDownloadDialog::kDefaultWindowHeight,
          "the content fits into the default window height");
    check(section_titles(&dialog) ==
              QStringList({QStringLiteral("File"), QStringLiteral("Save")}),
          "the dialog has File and Save sections in order");

    // The size has its own row and is no longer inline with the file name.
    // The expected text comes from the shared formatter: the decimal
    // separator follows the locale.
    const QString size_text =
        QString::fromStdString(swiftdm::format_bytes(1500000, 3));
    const QStringList texts = label_texts(&dialog);
    check(texts.contains(QStringLiteral("Name:")) &&
              texts.contains(QStringLiteral("Size:")),
          "the File section has Name and Size rows");
    check(!texts.contains(QStringLiteral("File :")) &&
              !texts.contains(QStringLiteral("Size :")),
          "the old spaced labels are gone");
    check(texts.contains(size_text), "the size row shows the formatted size");
    bool inline_size = false;
    for (const QString& text : texts) {
        if (text.contains(QStringLiteral("(%1").arg(size_text)) ||
            text.contains(QStringLiteral("%1)").arg(size_text))) {
            inline_size = true;
        }
    }
    check(!inline_size, "the size is no longer shown inline with the file name");
    check(is_in_section(&dialog, QStringLiteral("File"), "confirm_file_name") &&
              is_in_section(&dialog, QStringLiteral("File"), "confirm_file_size"),
          "the name and size sit in the File section");

    ConfirmDownloadDialog unknown(QStringLiteral("movie.mp4"), 0,
                                  QStringLiteral("https://example.com/movie.mp4"),
                                  QStringLiteral("/tmp"), true);
    check(label_texts(&unknown).contains(QStringLiteral("Unknown size")),
          "an unknown size is shown");

    ConfirmDownloadDialog pending(QStringLiteral("fallback.bin"), 0,
                                  QStringLiteral("https://example.com/fallback.bin"),
                                  QStringLiteral("/tmp"), true);
    pending.set_probe_pending();
    QPushButton* download_button = nullptr;
    for (QPushButton* button : pending.findChildren<QPushButton*>()) {
        if (button->text() == QStringLiteral("Download Now")) download_button = button;
    }
    check(label_texts(&pending).contains(QStringLiteral("Checking…")),
          "pending probe shows a checking status");
    check(download_button && !download_button->isEnabled(),
          "Download Now is disabled while the probe is pending");
    pending.set_probe_result(QStringLiteral("movie.mp4"), 1500000);
    check(download_button && download_button->isEnabled(),
          "Download Now is enabled after probe metadata arrives");
    check(label_texts(&pending).contains(size_text),
          "probe result replaces the pending size");
    bool resolved_name_shown = false;
    for (const QLabel* label : pending.findChildren<QLabel*>()) {
        if (label->toolTip() == QStringLiteral("movie.mp4")) resolved_name_shown = true;
    }
    check(resolved_name_shown, "probe result replaces the fallback filename");

    // A long file name or URL never widens the window.
    ConfirmDownloadDialog long_dialog(long_name, 1500000, long_url, QStringLiteral("/tmp"),
                                      true);
    check(long_dialog.size() == dialog.size(),
          "a long file name does not change the window size");
    check(long_dialog.layout()->minimumSize().width() <= ConfirmDownloadDialog::kMinWindowWidth,
          "a long file name never forces the dialog wider than the minimum");
    check(long_dialog.minimumSizeHint().width() <= ConfirmDownloadDialog::kMinWindowWidth,
          "the dialog minimum size hint stays inside the fixed minimum");

    // The full file name stays available in the tooltip.
    bool tooltip_ok = false;
    for (const QLabel* label : long_dialog.findChildren<QLabel*>()) {
        if (label->toolTip() == long_name) tooltip_ok = true;
    }
    check(tooltip_ok, "the full file name is kept in the tooltip");

    // The shown file name fits into the label the layout assigned.
    const QLabel* file_label = nullptr;
    for (const QLabel* label : dialog.findChildren<QLabel*>()) {
        if (label->toolTip() == QStringLiteral("movie.mp4")) file_label = label;
    }
    check(file_label != nullptr, "the dialog has a file name label");
    if (file_label) {
        activate_layouts(&dialog);
        check(file_label->text() == QStringLiteral("movie.mp4"),
              "a short file name is shown in full");
        check(file_label->sizePolicy().horizontalPolicy() == QSizePolicy::Ignored,
              "the file name takes the width the form gives it");
    }

    // Every description shares the vertical center of the widget next to it.
    const QLabel* size_label = label_with_text(&dialog, size_text);
    const QComboBox* category_box =
        qobject_cast<QComboBox*>(named_widget(&dialog, "confirm_category"));
    const QLineEdit* url_edit = nullptr;
    const QLineEdit* location_edit = nullptr;
    for (const QLineEdit* edit : dialog.findChildren<QLineEdit*>()) {
        if (edit->isReadOnly()) {
            url_edit = edit;
        } else {
            location_edit = edit;
        }
    }
    struct Row {
        const char* name;
        const QWidget* description;
        const QWidget* value;
    };
    const Row rows[] = {
        {"Name", label_with_text(&dialog, "Name:"), file_label},
        {"Size", label_with_text(&dialog, "Size:"), size_label},
        {"URL", label_with_text(&dialog, "URL:"), url_edit},
        {"Category", label_with_text(&dialog, "Category:"), category_box},
        {"Location", label_with_text(&dialog, "Location:"), location_edit},
    };
    for (const Row& row : rows) {
        check(row.description != nullptr && row.value != nullptr,
              "the dialog has a description and a value for every row");
        if (!row.description || !row.value) continue;
        const double description_center = center_y(row.description, &dialog);
        const double value_center = center_y(row.value, &dialog);
        if (std::abs(description_center - value_center) > 1.0) {
            std::fprintf(stderr, "FAIL: the %s row is not aligned (%.1f vs %.1f)\n",
                         row.name, description_center, value_center);
            ++failures;
        }
    }

    check(is_in_section(&dialog, QStringLiteral("Save"), "confirm_url") &&
              is_in_section(&dialog, QStringLiteral("Save"), "confirm_category") &&
              is_in_section(&dialog, QStringLiteral("Save"), "confirm_location") &&
              is_in_section(&dialog, QStringLiteral("Save"), "confirm_browse"),
          "the URL, category and location sit in the Save section");
    check(named_widget(&dialog, "confirm_queue_box") == nullptr,
          "the inline queue selector is gone: the picker owns the choice");

    auto* cancel_button =
        qobject_cast<QPushButton*>(named_widget(&dialog, "confirm_cancel"));
    auto* queue_button =
        qobject_cast<QPushButton*>(named_widget(&dialog, "confirm_queue"));
    auto* download_button_widget =
        qobject_cast<QPushButton*>(named_widget(&dialog, "confirm_download"));
    check(cancel_button && cancel_button->text() == QStringLiteral("Cancel"),
          "Cancel is on the left side of the button row");
    check(queue_button && queue_button->text() == QStringLiteral("Add to queue"),
          "Add to queue is part of the button row");
    check(queue_button && !queue_button->isEnabled(),
          "Add to queue is disabled when no queue exists");
    check(named_widget(&dialog, "confirm_queue_hint") == nullptr,
          "the inline queue hint label is gone");
    check(download_button_widget && download_button_widget->text() == QStringLiteral("Download Now"),
          "Download Now is on the right side of the button row");
    check(download_button_widget && download_button_widget->isDefault(),
          "Download Now is the default button");

    // From a queue view the target is known: Add to queue uses it directly and
    // becomes the default action, without a picker.
    ConfirmDownloadDialog queued(QStringLiteral("movie.mp4"), 1500000,
                                 QStringLiteral("https://example.com/movie.mp4"),
                                 QStringLiteral("/tmp"), true);
    queued.set_queue_choices({{7, QStringLiteral("Batch"), 2},
                              {9, QStringLiteral("Parts"), 0}},
                             9);
    auto* queued_button =
        qobject_cast<QPushButton*>(named_widget(&queued, "confirm_queue"));
    check(queued.selected_queue_id() == 9,
          "the queue the download came from is the target");
    check(queued_button && queued_button->isEnabled(),
          "Add to queue is enabled when a queue exists");
    check(queued_button && queued_button->isDefault(),
          "a known target makes Add to queue the default");
    if (queued_button) queued_button->click();
    check(queued.queue_requested(), "Add to queue reports the queue action");
    check(queued.selected_queue_id() == 9, "the known queue is kept");
    check(queued.result() == QDialog::Accepted, "Add to queue accepts the dialog");

    // With several queues and no target the button opens the picker, so the
    // dialog itself stays open and no queue is guessed.
    ConfirmDownloadDialog choose(QStringLiteral("movie.mp4"), 1500000,
                                 QStringLiteral("https://example.com/movie.mp4"),
                                 QStringLiteral("/tmp"), true);
    choose.set_queue_choices({{7, QStringLiteral("Batch"), 2},
                              {9, QStringLiteral("Parts"), 0}},
                             0);
    auto* choose_button =
        qobject_cast<QPushButton*>(named_widget(&choose, "confirm_queue"));
    check(choose.selected_queue_id() == 0, "no queue is guessed when several exist");
    check(choose_button && choose_button->isEnabled(),
          "Add to queue can open the picker");
    check(choose_button && !choose_button->isDefault(),
          "Download Now stays the default without a target");

    // A single queue leaves nothing to choose: Add to queue uses it directly.
    ConfirmDownloadDialog single(QStringLiteral("movie.mp4"), 1500000,
                                 QStringLiteral("https://example.com/movie.mp4"),
                                 QStringLiteral("/tmp"), true);
    single.set_queue_choices({{7, QStringLiteral("Batch"), 2}}, 0);
    auto* single_button =
        qobject_cast<QPushButton*>(named_widget(&single, "confirm_queue"));
    check(single_button && single_button->isEnabled(),
          "Add to queue is enabled for one queue");
    if (single_button) single_button->click();
    check(single.selected_queue_id() == 7,
          "the only queue is used without a picker");
    check(single.result() == QDialog::Accepted,
          "the single queue is accepted directly");

    // Download Now always targets the main list, even after a target was set.
    ConfirmDownloadDialog main_list(QStringLiteral("movie.mp4"), 1500000,
                                    QStringLiteral("https://example.com/movie.mp4"),
                                    QStringLiteral("/tmp"), true);
    main_list.set_queue_choices({{7, QStringLiteral("Batch"), 2},
                                 {9, QStringLiteral("Parts"), 0}},
                                9);
    auto* main_download =
        qobject_cast<QPushButton*>(named_widget(&main_list, "confirm_download"));
    if (main_download) main_download->click();
    check(main_list.selected_queue_id() == 0,
          "Download Now clears the queue target");
    check(!main_list.queue_requested(), "Download Now is not a queue action");

    // Every input and button shares one height.
    QList<int> control_heights;
    for (const QWidget* widget : dialog.findChildren<QWidget*>()) {
        if (qobject_cast<const QPushButton*>(widget) ||
            qobject_cast<const QLineEdit*>(widget) ||
            qobject_cast<const QComboBox*>(widget)) {
            control_heights << widget->height();
        }
    }
    check(control_heights.size() == 7, "the dialog has seven controls");
    bool same_height = !control_heights.isEmpty();
    for (const int height : control_heights) {
        if (height != control_heights.first()) same_height = false;
    }
    check(same_height, "every input and button has the same height");

    // The default location is shared with the automatic download path: the
    // download root plus the category folder when category folders are on.
    using swiftdm::ui::default_download_location;
    check(default_download_location(QStringLiteral("/tmp/downloads"),
                                   QStringLiteral("Video"), true) ==
              QStringLiteral("/tmp/downloads/Video"),
          "the default location holds the category folder");
    check(default_download_location(QStringLiteral("/tmp/downloads"),
                                   QStringLiteral("Video"), false) ==
              QStringLiteral("/tmp/downloads"),
          "the default location is the download root without category folders");
    check(default_download_location(QStringLiteral("  "), QStringLiteral("Video"),
                                   false) == QDir::homePath(),
          "an empty download root falls back to the home folder");

    if (failures == 0) std::printf("confirm_dialog: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
