// Headless check of the settings window structure: five tabs in a fixed order
// and every control on the tab that owns it. The dialog is never shown, so
// this stays within the CLI/backend-only test policy (AGENTS.md §6).

#include <QApplication>
#include <QCheckBox>
#include <QLabel>
#include <QLineEdit>
#include <QRadioButton>
#include <QSpinBox>
#include <QStringList>
#include <QTabWidget>

#include <cstdio>

#include "store/settings.h"
#include "ui/settings_dialog.h"

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

const QTabWidget* tabs_of(const QWidget* root) {
    return root->findChild<const QTabWidget*>();
}

// Index of the tab that holds `widget`, or -1 when the widget is not inside
// any tab page.
int tab_index_of(const QTabWidget* tabs, const QWidget* widget) {
    for (int i = 0; i < tabs->count(); ++i) {
        const QWidget* page = tabs->widget(i);
        if (page && page->isAncestorOf(widget)) return i;
    }
    return -1;
}

const QSpinBox* spin_box_with_maximum(const QWidget* root, int maximum) {
    for (const QSpinBox* box : root->findChildren<QSpinBox*>()) {
        if (box->maximum() == maximum) return box;
    }
    return nullptr;
}

const QCheckBox* check_box_with_text(const QWidget* root, const QString& text) {
    for (const QCheckBox* box : root->findChildren<QCheckBox*>()) {
        if (box->text() == text) return box;
    }
    return nullptr;
}

const QLineEdit* line_edit_with_text(const QWidget* root, const QString& text) {
    for (const QLineEdit* edit : root->findChildren<QLineEdit*>()) {
        if (edit->text() == text) return edit;
    }
    return nullptr;
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);

    swiftdm::Settings settings = swiftdm::default_settings();
    settings.config_path = "/tmp/swiftdm-settings-dialog-test.ini";
    settings.max_connections = 13;
    settings.completion_dialog = false;
    settings.autostart = true;
    settings.proxy.type = "http";
    settings.proxy.port = 3128;
    settings.default_agent = "TestAgent/2.0";
    settings.download_dir = "/tmp/swiftdm-dialog-downloads";

    swiftdm::ui::SettingsDialog dialog(settings);

    const QTabWidget* tabs = tabs_of(&dialog);
    check(tabs != nullptr, "the dialog has a tab widget");
    if (!tabs) return 1;

    check(tabs->count() == 5, "the dialog has five tabs");
    const QStringList expected = {QStringLiteral("General"), QStringLiteral("Interface"),
                                   QStringLiteral("Download"), QStringLiteral("Proxy/Socks"),
                                   QStringLiteral("Passwords")};
    for (int i = 0; i < tabs->count() && i < expected.size(); ++i) {
        check(tabs->tabText(i) == expected[i], "the tabs keep the expected order");
    }

    // General tab: the completion dialog and the autostart options.
    const QCheckBox* completion =
        check_box_with_text(&dialog, QStringLiteral("Show Download Completion Dialog"));
    const QCheckBox* autostart =
        check_box_with_text(&dialog, QStringLiteral("Start Application on System Startup"));
    check(completion != nullptr && autostart != nullptr,
          "the general tab has the completion dialog and autostart options");
    check(tab_index_of(tabs, completion) == 0, "the completion dialog sits on the general tab");
    check(tab_index_of(tabs, autostart) == 0, "the autostart option sits on the general tab");
    if (completion) check(!completion->isChecked(), "the completion dialog value is loaded");
    if (autostart) check(autostart->isChecked(), "the autostart value is loaded");

    // Download tab: connections, user agent and the directories.
    const QSpinBox* max_connections = spin_box_with_maximum(&dialog, 64);
    check(max_connections != nullptr, "the dialog has a max connections spin box");
    check(tab_index_of(tabs, max_connections) == 2,
          "the max connections spin box sits on the download tab");
    if (max_connections) check(max_connections->value() == 13, "the max connections value is loaded");

    const QLineEdit* agent = line_edit_with_text(&dialog, QStringLiteral("TestAgent/2.0"));
    check(agent != nullptr, "the dialog has the default user agent field");
    check(tab_index_of(tabs, agent) == 2, "the user agent field sits on the download tab");

    const QLineEdit* download_dir =
        line_edit_with_text(&dialog, QStringLiteral("/tmp/swiftdm-dialog-downloads"));
    check(download_dir != nullptr, "the dialog has the download directory field");
    check(tab_index_of(tabs, download_dir) == 2,
          "the download directory field sits on the download tab");

    // Interface tab: still a placeholder, but it must not hold download
    // controls and must not be empty.
    const QWidget* interface_page = tabs->widget(1);
    check(interface_page != nullptr, "the interface tab exists");
    if (interface_page) {
        check(!interface_page->findChildren<QLabel*>().isEmpty(),
              "the interface tab shows a placeholder");
    }
    check(tab_index_of(tabs, max_connections) != 1,
          "the interface tab holds no download controls");

    // Proxy tab: the port field stays on the proxy tab.
    const QSpinBox* proxy_port = spin_box_with_maximum(&dialog, 65535);
    check(proxy_port != nullptr, "the dialog has a proxy port spin box");
    check(tab_index_of(tabs, proxy_port) == 3, "the proxy port sits on the proxy tab");
    if (proxy_port) check(proxy_port->value() == 3128, "the proxy port value is loaded");

    // Passwords tab: the placeholder note.
    const QWidget* passwords_page = tabs->widget(4);
    check(passwords_page != nullptr && !passwords_page->findChildren<QLabel*>().isEmpty(),
          "the passwords tab shows its placeholder");

    if (failures == 0) std::printf("settings_dialog: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
