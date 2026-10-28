// Headless check of the settings window structure: five tabs in a fixed order,
// the labeled sections of every tab, and every control on the tab that owns it.
// The dialog is never shown, so this stays within the CLI/backend-only test
// policy (AGENTS.md §6).

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QGroupBox>
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

// Titles of the sections on a tab page, in creation/layout order.
QStringList section_titles(const QWidget* page) {
    QStringList titles;
    for (const QGroupBox* box : page->findChildren<QGroupBox*>()) {
        titles << box->title();
    }
    return titles;
}

const QGroupBox* section_with_title(const QWidget* page, const QString& title) {
    for (const QGroupBox* box : page->findChildren<QGroupBox*>()) {
        if (box->title() == title) return box;
    }
    return nullptr;
}

const QRadioButton* radio_with_text(const QWidget* root, const QString& text) {
    for (const QRadioButton* button : root->findChildren<QRadioButton*>()) {
        if (button->text() == text) return button;
    }
    return nullptr;
}

bool has_section_ancestor(const QWidget* widget) {
    for (const QWidget* parent = widget->parentWidget(); parent;
         parent = parent->parentWidget()) {
        if (qobject_cast<const QGroupBox*>(parent)) return true;
    }
    return false;
}

// Every interactive control of a tab must live inside a section, so no control
// is left flat or floats outside the grouped layout.
bool all_controls_are_in_sections(const QWidget* page) {
    for (const QWidget* widget : page->findChildren<QWidget*>()) {
        if (qobject_cast<const QCheckBox*>(widget) ||
            qobject_cast<const QSpinBox*>(widget) ||
            qobject_cast<const QLineEdit*>(widget) ||
            qobject_cast<const QComboBox*>(widget) ||
            qobject_cast<const QRadioButton*>(widget)) {
            if (!has_section_ancestor(widget)) return false;
        }
    }
    return true;
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
    settings.toolbar_size = "small";
    settings.show_toolbar_text = false;
    settings.theme = "light";
    settings.tray_icon_style = "simple";
    settings.show_tray_icon = false;
    settings.close_to_tray = false;
    settings.open_folder_after_completion = true;
    settings.show_download_confirmation = false;

    swiftdm::ui::SettingsDialog dialog(settings);

    const QTabWidget* tabs = tabs_of(&dialog);
    check(tabs != nullptr, "the dialog has a tab widget");
    if (!tabs) return 1;

    check(tabs->count() == 4, "the dialog has four tabs");
    // The section layout owns the minimum size. A fixed minimum below the
    // layout minimum lets Qt compress rows, which once collapsed the directory
    // editors to zero height.
    check(dialog.minimumSize().isNull(),
          "the dialog leaves the minimum size to the layout");
    const QStringList expected = {QStringLiteral("General"), QStringLiteral("Interface"),
                                   QStringLiteral("Download"), QStringLiteral("Proxy/Socks")};
    for (int i = 0; i < tabs->count() && i < expected.size(); ++i) {
        check(tabs->tabText(i) == expected[i], "the tabs keep the expected order");
    }

    // Every tab groups its options into titled sections, and no interactive
    // control is left outside a section.
    const QWidget* general_page = tabs->widget(0);
    const QWidget* interface_page = tabs->widget(1);
    const QWidget* download_page = tabs->widget(2);
    const QWidget* proxy_page = tabs->widget(3);
    check(general_page && interface_page && download_page && proxy_page,
          "every settings tab exists");
    if (general_page) {
        check(section_titles(general_page) ==
                  QStringList({QStringLiteral("Downloads"), QStringLiteral("Startup"),
                               QStringLiteral("System tray")}),
              "the general tab has the Downloads, Startup and System tray sections");
        check(all_controls_are_in_sections(general_page),
              "the general tab has no control outside a section");
    }
    if (interface_page) {
        check(section_titles(interface_page) ==
                  QStringList({QStringLiteral("Toolbar"), QStringLiteral("Appearance"),
                               QStringLiteral("System tray")}),
              "the interface tab has the Toolbar, Appearance and System tray sections");
        check(all_controls_are_in_sections(interface_page),
              "the interface tab has no control outside a section");
    }
    if (download_page) {
        check(section_titles(download_page) ==
                  QStringList({QStringLiteral("Connections"), QStringLiteral("Bandwidth"),
                               QStringLiteral("User-Agent"),
                               QStringLiteral("Files and folders")}),
              "the download tab has the Connections, Bandwidth, User-Agent and "
              "Files and folders sections");
        check(all_controls_are_in_sections(download_page),
              "the download tab has no control outside a section");
    }
    if (proxy_page) {
        check(section_titles(proxy_page) ==
                  QStringList({QStringLiteral("Proxy type"), QStringLiteral("Server"),
                               QStringLiteral("Authentication")}),
              "the proxy tab has the Proxy type, Server and Authentication sections");
        check(all_controls_are_in_sections(proxy_page),
              "the proxy tab has no control outside a section");
    }

    // General tab: the confirmation dialog, the completion dialog and the app
    // behavior options.
    const QCheckBox* confirmation = check_box_with_text(
        &dialog, QStringLiteral("Show download confirmation dialog"));
    const QCheckBox* completion =
        check_box_with_text(&dialog, QStringLiteral("Show download completion dialog"));
    const QCheckBox* autostart =
        check_box_with_text(&dialog, QStringLiteral("Start application on system startup"));
    const QCheckBox* tray_icon =
        check_box_with_text(&dialog, QStringLiteral("Show system tray icon"));
    const QCheckBox* close_to_tray = check_box_with_text(
        &dialog, QStringLiteral("Minimize to tray when closing the window"));
    const QCheckBox* open_folder = check_box_with_text(
        &dialog, QStringLiteral("Open the download folder when a download completes"));
    check(completion != nullptr && autostart != nullptr,
          "the general tab has the completion dialog and autostart options");
    check(confirmation != nullptr, "the general tab has the confirmation option");
    check(tray_icon != nullptr && close_to_tray != nullptr && open_folder != nullptr,
          "the general tab has the tray, close and folder options");
    check(tab_index_of(tabs, confirmation) == 0, "the confirmation option sits on the general tab");
    check(tab_index_of(tabs, completion) == 0, "the completion dialog sits on the general tab");
    check(tab_index_of(tabs, autostart) == 0, "the autostart option sits on the general tab");
    check(tab_index_of(tabs, tray_icon) == 0, "the tray option sits on the general tab");
    check(tab_index_of(tabs, close_to_tray) == 0, "the close option sits on the general tab");
    check(tab_index_of(tabs, open_folder) == 0, "the folder option sits on the general tab");
    if (general_page) {
        const QGroupBox* downloads = section_with_title(general_page, QStringLiteral("Downloads"));
        const QGroupBox* startup = section_with_title(general_page, QStringLiteral("Startup"));
        const QGroupBox* system_tray =
            section_with_title(general_page, QStringLiteral("System tray"));
        check(downloads && confirmation && completion && open_folder,
              "the Downloads section holds the download behavior options");
        if (downloads) {
            check(downloads->isAncestorOf(confirmation) &&
                      downloads->isAncestorOf(completion) &&
                      downloads->isAncestorOf(open_folder),
                  "the download behavior options sit in the Downloads section");
        }
        if (startup) {
            check(autostart && startup->isAncestorOf(autostart),
                  "the autostart option sits in the Startup section");
        }
        if (system_tray) {
            check(tray_icon && close_to_tray && system_tray->isAncestorOf(tray_icon) &&
                      system_tray->isAncestorOf(close_to_tray),
                  "the tray options sit in the System tray section");
        }
    }
    if (completion) check(!completion->isChecked(), "the completion dialog value is loaded");
    if (confirmation) check(!confirmation->isChecked(), "the confirmation value is loaded");

    // The note under the confirmation option explains the automatic download
    // and is only visible while the option is off.
    const QLabel* note = nullptr;
    for (const QLabel* label : dialog.findChildren<QLabel*>()) {
        if (label->text().contains(QStringLiteral("downloaded automatically"))) note = label;
    }
    check(note != nullptr, "the general tab explains the automatic download");
    if (note) {
        check(tab_index_of(tabs, note) == 0, "the note sits on the general tab");
        // The dialog is never shown, so the explicit hidden state is checked
        // instead of the visible one.
        check(!note->isHidden(), "the note is shown while the option is off");
    }
    {
        // Toggling the option shows and hides the note again.
        QCheckBox* box = nullptr;
        for (QCheckBox* candidate : dialog.findChildren<QCheckBox*>()) {
            if (candidate->text() == QStringLiteral("Show download confirmation dialog")) {
                box = candidate;
            }
        }
        check(box != nullptr, "the confirmation option can be toggled");
        if (box && note) {
            box->setChecked(true);
            check(note->isHidden(), "the note is hidden while the option is on");
            box->setChecked(false);
            check(!note->isHidden(), "the note is shown again while the option is off");
        }
    }
    if (autostart) check(autostart->isChecked(), "the autostart value is loaded");
    if (tray_icon) check(!tray_icon->isChecked(), "the tray icon value is loaded");
    if (close_to_tray) check(!close_to_tray->isChecked(), "the close to tray value is loaded");
    if (open_folder) check(open_folder->isChecked(), "the open folder value is loaded");

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

    if (download_page) {
        const QGroupBox* connections =
            section_with_title(download_page, QStringLiteral("Connections"));
        const QGroupBox* user_agent =
            section_with_title(download_page, QStringLiteral("User-Agent"));
        const QGroupBox* files =
            section_with_title(download_page, QStringLiteral("Files and folders"));
        if (connections) {
            check(max_connections && connections->isAncestorOf(max_connections),
                  "the connections spin box sits in the Connections section");
        }
        if (user_agent) {
            check(agent && user_agent->isAncestorOf(agent),
                  "the user agent field sits in the User-Agent section");
        }
        if (files) {
            check(download_dir && files->isAncestorOf(download_dir),
                  "the download directory sits in the Files and folders section");
        }

        // Auto-exclusive radio buttons only work as siblings, so both halves of
        // the connection policy must share one parent inside the section.
        const QRadioButton* all_at_once = radio_with_text(
            download_page, QStringLiteral("Create additional connections all at once"));
        const QRadioButton* one_by_one = radio_with_text(
            download_page, QStringLiteral("Create additional connections one by one"));
        check(all_at_once && one_by_one &&
                  all_at_once->parentWidget() == one_by_one->parentWidget(),
              "the connection policy radio buttons share a parent");
    }

    // Interface tab: the appearance options.
    if (interface_page) {
        const auto combos = interface_page->findChildren<QComboBox*>();
        const auto boxes = interface_page->findChildren<QCheckBox*>();
        check(combos.size() == 3, "the interface tab has three selectors");
        check(boxes.size() == 1, "the interface tab has one checkbox");

        QStringList texts;
        for (const QComboBox* combo : combos) texts << combo->currentText();
        check(texts.contains(QStringLiteral("Small")),
              "the toolbar size selector shows the stored value");
        check(texts.contains(QStringLiteral("Light")),
              "the theme selector shows the stored value");
        check(texts.contains(QStringLiteral("Simple")),
              "the tray icon selector shows the stored value");
        for (const QCheckBox* box : boxes) {
            check(!box->isChecked(), "the toolbar text checkbox shows the stored value");
        }

        const QGroupBox* toolbar = section_with_title(interface_page, QStringLiteral("Toolbar"));
        const QGroupBox* appearance =
            section_with_title(interface_page, QStringLiteral("Appearance"));
        const QGroupBox* system_tray =
            section_with_title(interface_page, QStringLiteral("System tray"));
        if (toolbar) {
            check(toolbar->findChildren<QComboBox*>().size() == 1 &&
                      toolbar->findChildren<QCheckBox*>().size() == 1,
                  "the toolbar section holds the size selector and text checkbox");
        }
        if (appearance) {
            check(appearance->findChildren<QComboBox*>().size() == 1,
                  "the appearance section holds the theme selector");
        }
        if (system_tray) {
            check(system_tray->findChildren<QComboBox*>().size() == 1,
                  "the system tray section holds the icon style selector");
        }
    }
    check(tab_index_of(tabs, max_connections) != 1,
          "the interface tab holds no download controls");

    // Proxy tab: the port field stays on the proxy tab.
    const QSpinBox* proxy_port = spin_box_with_maximum(&dialog, 65535);
    check(proxy_port != nullptr, "the dialog has a proxy port spin box");
    check(tab_index_of(tabs, proxy_port) == 3, "the proxy port sits on the proxy tab");
    if (proxy_port) check(proxy_port->value() == 3128, "the proxy port value is loaded");
    if (proxy_page) {
        const QGroupBox* server = section_with_title(proxy_page, QStringLiteral("Server"));
        const QGroupBox* authentication =
            section_with_title(proxy_page, QStringLiteral("Authentication"));
        if (server) {
            check(proxy_port && server->isAncestorOf(proxy_port),
                  "the proxy port sits in the Server section");
        }
        if (authentication) {
            check(authentication->findChildren<QLineEdit*>().size() == 2,
                  "the authentication section holds the user and password fields");
        }
        const QRadioButton* no_proxy =
            radio_with_text(proxy_page, QStringLiteral("No Proxy/Socks"));
        const QRadioButton* http_proxy =
            radio_with_text(proxy_page, QStringLiteral("HTTP Proxy"));
        const QRadioButton* socks_proxy =
            radio_with_text(proxy_page, QStringLiteral("Socks Proxy"));
        check(no_proxy && http_proxy && socks_proxy &&
                  no_proxy->parentWidget() == http_proxy->parentWidget() &&
                  no_proxy->parentWidget() == socks_proxy->parentWidget(),
              "the proxy type radio buttons share a parent");
    }

    if (failures == 0) std::printf("settings_dialog: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
