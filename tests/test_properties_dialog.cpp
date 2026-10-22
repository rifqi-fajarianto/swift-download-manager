// Headless check of the Download Properties window structure: the header, the
// four labeled sections in their fixed order, every value in the section that
// owns it, and the conditional source rows. The dialog is never shown, so this
// stays within the CLI/backend-only test policy (AGENTS.md §6).

#include <QApplication>
#include <QDialogButtonBox>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStringList>
#include <QTemporaryDir>

#include <cstdio>

#include "ui/download_model.h"
#include "ui/properties_dialog.h"

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

swiftdm::ui::DownloadItem make_item(const QString& directory) {
    using swiftdm::ui::DownloadItem;
    DownloadItem item;
    item.file_name = QStringLiteral("test_random_100mb.zip");
    item.url = QStringLiteral("http://speedtest.example.test/download/test_random_100mb.zip");
    item.category = QStringLiteral("Compressed");
    item.output_dir = directory;
    item.dest_path = directory + QStringLiteral("/test_random_100mb.zip");
    item.temp_path = directory + QStringLiteral("/tmp/download-6b6ab8ae96227020");
    item.size = 104857600;
    item.done = 67108864;
    item.state = DownloadItem::State::Paused;
    item.percent = 64;
    item.resumable = true;
    item.added = QStringLiteral("Oct 22 15:32:22");
    item.last_try = QStringLiteral("Oct 22 15:32:22");
    item.page_url = QStringLiteral("https://example.test/video");
    item.page_title = QStringLiteral("Example page");
    item.group = QStringLiteral("pair-6b6ab8ae");
    return item;
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);

    QTemporaryDir temp_dir;
    check(temp_dir.isValid(), "the test download folder exists");
    if (!temp_dir.isValid()) return 1;

    const swiftdm::ui::DownloadItem item = make_item(temp_dir.path());
    swiftdm::ui::PropertiesDialog dialog(item, nullptr);

    check(dialog.windowTitle() == QStringLiteral("Download Properties"),
          "the window title is Download Properties");
    check(dialog.minimumSize().isNull(), "the dialog leaves the minimum size to the layout");
    check(section_titles(&dialog) ==
              QStringList({QStringLiteral("General"), QStringLiteral("Location"),
                           QStringLiteral("Source"), QStringLiteral("Time")}),
          "the dialog has the General, Location, Source and Time sections in order");

    auto* name = qobject_cast<QLabel*>(named_widget(&dialog, "properties_file_name"));
    check(name != nullptr, "the header has the file name");
    if (name) {
        check(name->text() == item.file_name, "a short file name is shown in full");
        check(name->toolTip() == item.file_name, "the file name keeps the full name in its tooltip");
        check(name->font().bold(), "the file name is bold");
    }

    auto* file_size = qobject_cast<QLabel*>(named_widget(&dialog, "properties_file_size"));
    check(file_size != nullptr, "the general section has the file size");
    if (file_size) {
        check(file_size->text().contains(QStringLiteral("104857600")),
              "the file size keeps the raw byte count");
        check(file_size->text().contains(QStringLiteral("104.858 MB")) ||
                  file_size->text().contains(QStringLiteral("104,858 MB")),
              "the file size keeps the formatted size");
    }

    auto* status = qobject_cast<QLabel*>(named_widget(&dialog, "properties_status"));
    check(status != nullptr, "the general section has the status");
    if (status) {
        check(status->text() == QStringLiteral("Paused ( 64% )"), "the status text is loaded");
        check(status->styleSheet().contains(QStringLiteral("#5aa9e6")),
              "the paused status keeps the blue color");
    }

    auto* category = qobject_cast<QLabel*>(named_widget(&dialog, "properties_category"));
    check(category && category->text() == QStringLiteral("Compressed"),
          "the general section shows the category");

    auto* downloaded = qobject_cast<QLabel*>(named_widget(&dialog, "properties_downloaded"));
    check(downloaded && downloaded->text().contains(QStringLiteral("64%")),
          "the general section shows the downloaded percentage");

    auto* resumable = qobject_cast<QLabel*>(named_widget(&dialog, "properties_resumable"));
    check(resumable && resumable->text() == QStringLiteral("Yes"),
          "the general section shows the resumable state");

    auto* segments = qobject_cast<QLabel*>(named_widget(&dialog, "properties_segments"));
    check(segments && segments->text() == QStringLiteral("0"),
          "the segment count is read from the task snapshot");

    check(is_in_section(&dialog, QStringLiteral("General"), "properties_file_size") &&
              is_in_section(&dialog, QStringLiteral("General"), "properties_status") &&
              is_in_section(&dialog, QStringLiteral("General"), "properties_category") &&
              is_in_section(&dialog, QStringLiteral("General"), "properties_downloaded") &&
              is_in_section(&dialog, QStringLiteral("General"), "properties_resumable") &&
              is_in_section(&dialog, QStringLiteral("General"), "properties_segments"),
          "the general values sit in the General section");

    auto* saved_to = qobject_cast<QLineEdit*>(named_widget(&dialog, "properties_saved_to"));
    check(saved_to != nullptr, "the location section has the saved folder");
    if (saved_to) {
        check(saved_to->text() == temp_dir.path(), "the saved folder is the destination folder");
        check(saved_to->isReadOnly(), "the saved folder stays read-only");
    }
    auto* open_folder = qobject_cast<QPushButton*>(named_widget(&dialog, "properties_open_folder"));
    check(open_folder != nullptr, "the location section has the Open Folder button");
    if (open_folder) {
        check(open_folder->text() == QStringLiteral("Open Folder"),
              "the folder button keeps its label");
        check(open_folder->isEnabled(), "the folder button is enabled for an existing folder");
    }
    auto* temp_path = qobject_cast<QLineEdit*>(named_widget(&dialog, "properties_temp_path"));
    check(temp_path != nullptr, "the location section has the temp path");
    if (temp_path) {
        check(temp_path->text() == item.temp_path, "the temp path value is loaded");
        check(temp_path->isReadOnly(), "the temp path stays read-only");
    }
    check(is_in_section(&dialog, QStringLiteral("Location"), "properties_saved_to") &&
              is_in_section(&dialog, QStringLiteral("Location"), "properties_open_folder") &&
              is_in_section(&dialog, QStringLiteral("Location"), "properties_temp_path"),
          "the location values sit in the Location section");

    auto* url = qobject_cast<QLineEdit*>(named_widget(&dialog, "properties_url"));
    check(url != nullptr, "the source section has the URL");
    if (url) {
        check(url->text() == item.url, "the URL value is loaded");
        check(url->isReadOnly(), "the URL stays read-only");
    }
    auto* page_url = qobject_cast<QLineEdit*>(named_widget(&dialog, "properties_page_url"));
    check(page_url != nullptr, "a browser download shows the page URL");
    if (page_url) {
        check(page_url->text() == item.page_url, "the page URL value is loaded");
        check(page_url->isReadOnly(), "the page URL stays read-only");
    }
    auto* page_title = qobject_cast<QLabel*>(named_widget(&dialog, "properties_page_title"));
    check(page_title != nullptr, "a browser download shows the page title");
    if (page_title) {
        check(page_title->text() == item.page_title, "the page title value is loaded");
        check(page_title->wordWrap(), "the page title wraps instead of widening the window");
        check(page_title->sizePolicy().horizontalPolicy() == QSizePolicy::Ignored,
              "the page title takes the width the form gives it");
    }
    auto* pair = qobject_cast<QLineEdit*>(named_widget(&dialog, "properties_pair"));
    check(pair != nullptr, "a paired download shows its pair token");
    if (pair) {
        check(pair->text() == item.group, "the pair value is loaded");
        check(pair->isReadOnly(), "the pair value stays read-only");
    }
    check(is_in_section(&dialog, QStringLiteral("Source"), "properties_url") &&
              is_in_section(&dialog, QStringLiteral("Source"), "properties_page_url") &&
              is_in_section(&dialog, QStringLiteral("Source"), "properties_page_title") &&
              is_in_section(&dialog, QStringLiteral("Source"), "properties_pair"),
          "the source values sit in the Source section");

    auto* added_on = qobject_cast<QLabel*>(named_widget(&dialog, "properties_added_on"));
    auto* last_try = qobject_cast<QLabel*>(named_widget(&dialog, "properties_last_try"));
    check(added_on && added_on->text() == item.added, "the added timestamp is loaded");
    check(last_try && last_try->text() == item.last_try, "the last try timestamp is loaded");
    check(is_in_section(&dialog, QStringLiteral("Time"), "properties_added_on") &&
              is_in_section(&dialog, QStringLiteral("Time"), "properties_last_try"),
          "the timestamps sit in the Time section");

    auto* buttons = dialog.findChild<QDialogButtonBox*>();
    check(buttons != nullptr, "the dialog has a button box");
    if (buttons) {
        check(buttons->standardButtons() == QDialogButtonBox::Ok,
              "the button box only offers OK");
    }

    // Conditional rows only exist when the item carries that metadata.
    swiftdm::ui::DownloadItem bare = item;
    bare.page_url.clear();
    bare.page_title.clear();
    bare.group.clear();
    swiftdm::ui::PropertiesDialog bare_dialog(bare, nullptr);
    check(named_widget(&bare_dialog, "properties_page_url") == nullptr,
          "a direct download hides the page URL row");
    check(named_widget(&bare_dialog, "properties_page_title") == nullptr,
          "a direct download hides the page title row");
    check(named_widget(&bare_dialog, "properties_pair") == nullptr,
          "a single download hides the pair row");
    check(section_titles(&bare_dialog) ==
              QStringList({QStringLiteral("General"), QStringLiteral("Location"),
                           QStringLiteral("Source"), QStringLiteral("Time")}),
          "the four sections remain in order for a direct download");

    // A long name is elided in the middle and keeps the full name in a tooltip.
    swiftdm::ui::DownloadItem long_item = item;
    long_item.file_name =
        QStringLiteral("this-is-a-very-long-file-name-").repeated(6) + QStringLiteral("archive.zip");
    swiftdm::ui::PropertiesDialog long_dialog(long_item, nullptr);
    auto* long_name =
        qobject_cast<QLabel*>(named_widget(&long_dialog, "properties_file_name"));
    check(long_name != nullptr, "the long name dialog has the header name");
    if (long_name) {
        check(long_name->text() != long_item.file_name, "a long file name is elided");
        check(long_name->text().contains(QChar(0x2026)),
              "the elided name keeps a middle ellipsis");
        check(long_name->toolTip() == long_item.file_name,
              "the elided name keeps the full name in its tooltip");
    }

    if (failures == 0) std::printf("properties_dialog: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
