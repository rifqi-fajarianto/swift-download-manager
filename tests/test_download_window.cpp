// Headless check of the Download window structure: the three tabs, the labeled
// sections on each tab, and the fields that belong to them. The window is
// never shown, so this stays within the CLI/backend-only test policy
// (AGENTS.md §6).

#include <QApplication>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QStringList>
#include <QTabWidget>
#include <QTableWidget>

#include <cstdio>

#include "ui/download_model.h"
#include "ui/download_task.h"
#include "ui/download_window.h"
#include "ui/speed_limit_editor.h"

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
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

QWidget* named_widget(const QWidget* root, const char* name) {
    return root->findChild<QWidget*>(QString::fromLatin1(name));
}

bool is_in_section(const QWidget* root, const QString& title, const char* name) {
    const QGroupBox* section = section_with_title(root, title);
    const QWidget* widget = named_widget(root, name);
    return section && widget && section->isAncestorOf(widget);
}

QStringList label_texts(const QWidget* root) {
    QStringList texts;
    for (const QLabel* label : root->findChildren<QLabel*>()) texts << label->text();
    return texts;
}

swiftdm::ui::DownloadItem make_item(const QString& directory) {
    using swiftdm::ui::DownloadItem;
    DownloadItem item;
    item.file_name = QStringLiteral("test_random_10mb.zip");
    item.url = QStringLiteral("http://speedtest.example.test/test_random_10mb.zip");
    item.output_dir = directory;
    item.size = 10485760;
    item.done = 10485760;
    item.state = DownloadItem::State::Complete;
    item.percent = 100;
    item.resumable = true;
    item.bandwidth_limit_enabled = true;
    item.bandwidth_limit_kbps = 500;
    return item;
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);

    const QString directory = QStringLiteral("/tmp/swiftdm-download-window-test");
    swiftdm::ui::DownloadModel model;
    model.add_item(make_item(directory));
    const int item_id = model.item_at(0)->id;

    swiftdm::Settings settings;
    swiftdm::ui::DownloadWindow window(&model, item_id, settings);

    check(window.windowTitle() == QStringLiteral("Complete - test_random_10mb.zip"),
          "the title follows the status and file name");
    check(window.minimumWidth() == 500 && window.minimumHeight() == 400,
          "the window keeps its 520x420 minimum");
    check(window.size() == QSize(600, 460), "the window opens at 600x460");

    auto* tabs = window.findChild<QTabWidget*>();
    check(tabs != nullptr, "the window has a tab widget");
    if (!tabs) return 1;
    check(tabs->count() == 3, "the window has three tabs");
    check(tabs->tabText(0) == QStringLiteral("Download") &&
              tabs->tabText(1) == QStringLiteral("Options") &&
              tabs->tabText(2) == QStringLiteral("Connections"),
          "the tabs are Download, Options and Connections");

    QWidget* download_page = tabs->widget(0);
    QWidget* options_page = tabs->widget(1);
    QWidget* connections_page = tabs->widget(2);
    check(section_titles(download_page) ==
              QStringList({QStringLiteral("Details"), QStringLiteral("Progress")}),
          "the Download tab has Details and Progress sections in order");
    check(section_titles(options_page) ==
              QStringList({QStringLiteral("Destination"), QStringLiteral("Transfer")}),
          "the Options tab has Destination and Transfer sections in order");
    check(section_titles(connections_page) == QStringList({QString()}),
          "the Connections tab has one untitled frame");

    const QStringList download_labels = label_texts(download_page);
    check(download_labels.contains(QStringLiteral("Status")) &&
              download_labels.contains(QStringLiteral("File size")) &&
              download_labels.contains(QStringLiteral("Downloaded")) &&
              download_labels.contains(QStringLiteral("Bandwidth")) &&
              download_labels.contains(QStringLiteral("Remaining time")) &&
              download_labels.contains(QStringLiteral("Resumable")),
          "the Details labels use sentence case");
    check(!download_labels.contains(QStringLiteral("File Size")),
          "the old File Size capitalization is gone");

    auto* status = qobject_cast<QLabel*>(named_widget(&window, "download_status"));
    check(status && status->text() == QStringLiteral("Complete"),
          "the status value is loaded");
    auto* file_size = qobject_cast<QLabel*>(named_widget(&window, "download_file_size"));
    check(file_size && file_size->text().contains(QStringLiteral("10")),
          "the file size value is loaded");
    auto* downloaded = qobject_cast<QLabel*>(named_widget(&window, "download_downloaded"));
    check(downloaded && downloaded->text().contains(QStringLiteral("100%")),
          "the downloaded value includes the percentage");
    auto* bandwidth = qobject_cast<QLabel*>(named_widget(&window, "download_bandwidth"));
    check(bandwidth != nullptr, "the bandwidth value exists");
    auto* limit_info = qobject_cast<QLabel*>(named_widget(&window, "download_limit_info"));
    check(limit_info && limit_info->text() == QStringLiteral("Speed limit: 500 KB/s"),
          "the active speed limit is shown next to the bandwidth");
    auto* resumable = qobject_cast<QLabel*>(named_widget(&window, "download_resumable"));
    check(resumable && resumable->text() == QStringLiteral("Yes"),
          "the resumable value is loaded");

    check(is_in_section(download_page, QStringLiteral("Details"), "download_status") &&
              is_in_section(download_page, QStringLiteral("Details"), "download_bandwidth") &&
              is_in_section(download_page, QStringLiteral("Details"), "download_limit_info") &&
              is_in_section(download_page, QStringLiteral("Details"), "download_resumable"),
          "the detail values sit in the Details section");

    auto* progress = qobject_cast<QProgressBar*>(named_widget(&window, "download_progress"));
    check(progress != nullptr, "the Progress section has a progress bar");
    if (progress) {
        check(progress->isTextVisible(), "the progress bar shows its percentage");
        check(progress->value() == 100, "the progress bar follows the item");
    }
    check(named_widget(&window, "download_segment_bar") != nullptr,
          "the Progress section has a segment bar");
    auto* segments = qobject_cast<QLabel*>(named_widget(&window, "download_segments"));
    check(segments && segments->text() == QStringLiteral("Segments: 0"),
          "the segment count uses the mockup spelling");
    check(is_in_section(download_page, QStringLiteral("Progress"), "download_progress") &&
              is_in_section(download_page, QStringLiteral("Progress"), "download_segment_bar") &&
              is_in_section(download_page, QStringLiteral("Progress"), "download_segments"),
          "the progress widgets sit in the Progress section");

    auto* action = qobject_cast<QPushButton*>(named_widget(&window, "download_action"));
    auto* cancel = qobject_cast<QPushButton*>(named_widget(&window, "download_cancel"));
    check(action && action->text() == QStringLiteral("Resume"),
          "a complete item offers Resume");
    check(cancel && cancel->text() == QStringLiteral("Cancel"),
          "the window has a Cancel button");

    const QStringList option_labels = label_texts(options_page);
    check(option_labels.contains(QStringLiteral("Save to:")) &&
              option_labels.contains(QStringLiteral("File name:")),
          "the Destination labels keep the form colons");
    check(option_labels.contains(QStringLiteral("Connections:")) &&
              option_labels.contains(QStringLiteral("Bandwidth limit:")) &&
              option_labels.contains(QStringLiteral("Referer:")),
          "the Transfer labels keep the form colons");

    auto* save_to = qobject_cast<QLineEdit*>(named_widget(&window, "download_save_to"));
    check(save_to && save_to->text() == directory, "the destination folder is loaded");
    auto* file_name = qobject_cast<QLineEdit*>(named_widget(&window, "download_file_name"));
    check(file_name && file_name->text() == QStringLiteral("test_random_10mb.zip"),
          "the file name is loaded");
    check(is_in_section(options_page, QStringLiteral("Destination"), "download_save_to") &&
              is_in_section(options_page, QStringLiteral("Destination"), "download_file_name"),
          "the destination fields sit in the Destination section");

    auto* connections = qobject_cast<QSpinBox*>(named_widget(&window, "download_connections"));
    check(connections != nullptr, "the Transfer section has a connection count");
    auto* limit_editor =
        qobject_cast<swiftdm::ui::SpeedLimitEditor*>(named_widget(&window, "download_bandwidth_limit"));
    check(limit_editor != nullptr, "the Transfer section has the speed limit editor");
    if (limit_editor) {
        const auto parts = limit_editor->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly);
        bool one_row = !parts.isEmpty();
        for (QWidget* part : parts) one_row = one_row && part->y() == parts.first()->y();
        check(one_row, "the speed limit editor toggle, value and preset share one row");
    }
    auto* referer = qobject_cast<QLineEdit*>(named_widget(&window, "download_referer"));
    check(referer != nullptr, "the Transfer section has a referer field");
    auto* note = qobject_cast<QLabel*>(named_widget(&window, "download_transfer_note"));
    check(note != nullptr, "the Transfer section has its note");
    if (note) {
        check(note->text() ==
                  QStringLiteral("Speed limit changes apply while downloading. "
                                 "Other changes apply when resuming."),
              "the note explains when the changes apply");
    }
    check(is_in_section(options_page, QStringLiteral("Transfer"), "download_connections") &&
              is_in_section(options_page, QStringLiteral("Transfer"), "download_bandwidth_limit") &&
              is_in_section(options_page, QStringLiteral("Transfer"), "download_referer") &&
              is_in_section(options_page, QStringLiteral("Transfer"), "download_transfer_note"),
          "the transfer fields sit in the Transfer section");

    auto* apply = qobject_cast<QPushButton*>(named_widget(&window, "download_apply"));
    check(apply && apply->text() == QStringLiteral("Apply"),
          "the Options tab has an Apply button");

    auto* table = qobject_cast<QTableWidget*>(named_widget(&window, "download_connections_table"));
    check(table != nullptr, "the Connections tab has a table");
    if (table) {
        check(table->columnCount() == 4, "the table has four columns");
        check(table->horizontalHeaderItem(0)->text() == QStringLiteral("#") &&
                  table->horizontalHeaderItem(1)->text() == QStringLiteral("Range (bytes)") &&
                  table->horizontalHeaderItem(2)->text() == QStringLiteral("Downloaded") &&
                  table->horizontalHeaderItem(3)->text() == QStringLiteral("%"),
              "the table headers match the mockup");
        check(!table->showGrid(), "the table leaves the grid to the section frame");
        check(table->frameShape() == QFrame::NoFrame,
              "the table does not repeat the section border");
        check(table->rowCount() == 0, "a restored item starts without live segments");
    }
    check(is_in_section(connections_page, QString(), "download_connections_table"),
          "the table sits inside the Connections frame");

    // A queue item shares the queue budget: the per-item editor stays visible
    // for reference but is disabled, and the Transfer section explains the
    // queue context.
    auto* queue_note =
        qobject_cast<QLabel*>(named_widget(&window, "download_queue_limit_note"));
    check(queue_note && queue_note->isHidden(),
          "the queue note is hidden for a main-list item");
    if (auto* item = model.item_at(0)) {
        item->state = swiftdm::ui::DownloadItem::State::Paused;
        item->connections = 4;
        swiftdm::ui::DownloadTask::Options options;
        options.url = item->url;
        options.output_dir = directory;
        item->task = new swiftdm::ui::DownloadTask(settings, options, &model);
        model.touch_row(0);
    }
    check(limit_editor && limit_editor->isEnabled() && connections &&
              connections->isEnabled(),
          "an incomplete main-list item keeps its per-item limiter and connections");
    window.set_queue_context(QStringLiteral("Batch"), true, 2048);
    check(limit_info && limit_info->text() == QStringLiteral("Queue limit: 2 MB/s"),
          "the queue budget replaces the per-item limit label");
    check(connections && connections->value() == 4,
          "the queue context keeps the row's stored per-item connection count");
    check(limit_editor && !limit_editor->isEnabled(),
          "the per-item limiter is disabled for a queue item");
    check(connections && !connections->isEnabled(),
          "the per-item connection count is disabled for a queue item");
    check(queue_note && !queue_note->isHidden() &&
              queue_note->text().contains(QStringLiteral("Batch")) &&
              queue_note->text().contains(QStringLiteral("2 MB/s")),
          "the queue note names the queue and its shared limit");
    check(is_in_section(options_page, QStringLiteral("Transfer"),
                        "download_queue_limit_note"),
          "the queue note sits in the Transfer section");
    window.set_queue_context(QStringLiteral("Batch"), false, 0);
    check(limit_info && limit_info->text().isEmpty(),
          "an unlimited queue clears the limit label");
    check(queue_note && queue_note->text().contains(
                            QStringLiteral("queue controls both")),
          "an unlimited queue still disables the per-item editors");
    window.clear_queue_context();
    check(queue_note && queue_note->isHidden(),
          "moving back to Downloads hides the queue note");
    check(limit_editor && limit_editor->isEnabled() && connections &&
              connections->isEnabled(),
          "moving back to Downloads restores the per-item editors");
    check(connections && connections->value() == 4,
          "moving back to Downloads preserves the stored connection count");

    if (failures == 0) std::printf("download_window: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
