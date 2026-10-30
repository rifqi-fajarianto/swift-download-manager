#include "ui/settings_dialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QVBoxLayout>

#include "core/util.h"
#include "engine/categories.h"
#include "swiftdm/version.h"
#include "ui/confirm_download_dialog.h"
#include "ui/icons.h"

namespace swiftdm::ui {

namespace {

// Consistent spacing between the sections of every tab.
constexpr int kSectionSpacing = 8;

// Adds an empty section to a tab page and returns the layout of its body.
QVBoxLayout* add_section(QVBoxLayout* page_layout, QWidget* page, const QString& title) {
    auto* box = new QGroupBox(title, page);
    auto* body = new QVBoxLayout(box);
    body->setContentsMargins(10, 4, 10, 8);
    body->setSpacing(6);
    page_layout->addWidget(box);
    return body;
}

// Adds a label/field section. All forms share the same alignment and spacing so
// the rows line up across sections and tabs.
QFormLayout* add_form_section(QVBoxLayout* page_layout, QWidget* page,
                              const QString& title) {
    auto* box = new QGroupBox(title, page);
    auto* form = new QFormLayout(box);
    form->setContentsMargins(10, 4, 10, 8);
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    form->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    form->setHorizontalSpacing(12);
    form->setVerticalSpacing(6);
    page_layout->addWidget(box);
    return form;
}

}  // namespace

SettingsDialog::SettingsDialog(swiftdm::Settings& settings, QWidget* parent)
    : QDialog(parent), settings_(settings) {
    setWindowTitle(tr("SwiftDM Settings"));
    // The sections need more room than the previous flat form. The layout also
    // provides the minimum size, so a shorter window would compress the rows
    // instead of scrolling; do not override it with setMinimumSize().
    resize(720, 580);

    auto* tabs = new QTabWidget(this);
    tabs->addTab(build_general_tab(), tr("General"));
    tabs->addTab(build_interface_tab(), tr("Interface"));
    tabs->addTab(build_download_tab(), tr("Download"));
    tabs->addTab(build_proxy_tab(), tr("Proxy/Socks"));

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        store_to_settings();
        std::string error;
        if (!swiftdm::save_settings(settings_, &error)) {
            QMessageBox box(this);
            box.setIconPixmap(dialog_warning_pixmap());
            box.setWindowTitle(tr("Settings"));
            box.setText(tr("Could not save settings: %1")
                            .arg(QString::fromStdString(error)));
            box.exec();
        }
        std::string autostart_error;
        if (!swiftdm::apply_autostart(settings_.autostart, &autostart_error)) {
            QMessageBox box(this);
            box.setIconPixmap(dialog_warning_pixmap());
            box.setWindowTitle(tr("Settings"));
            box.setText(tr("Could not update autostart entry: %1")
                            .arg(QString::fromStdString(autostart_error)));
            box.exec();
        }
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(tabs, 1);
    layout->addWidget(buttons);

    load_from_settings();
}

// Adds a directory picker row to `layout`; returns the line edit. The row
// widgets are parented to `parent` so they stay inside the section that owns
// the form layout. `dialog_title` labels the folder chooser; it defaults to
// the row label, which carries a trailing colon for the form.
QLineEdit* SettingsDialog::add_directory_row(QFormLayout* layout, QWidget* parent,
                                             const QString& label,
                                             const QString& dialog_title) {
    auto* container = new QWidget(parent);
    auto* row = new QHBoxLayout(container);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(6);
    auto* edit = new QLineEdit(container);
    auto* browse = new QPushButton("...", container);
    browse->setFixedWidth(32);
    row->addWidget(edit, 1);
    row->addWidget(browse);
    layout->addRow(label, container);
    const QString chooser_title = dialog_title.isEmpty() ? label : dialog_title;
    connect(browse, &QPushButton::clicked, this, [this, edit, chooser_title] {
        const QString start =
            QString::fromStdString(swiftdm::expand_tilde(edit->text().toStdString()));
        const QString dir = QFileDialog::getExistingDirectory(this, chooser_title, start);
        if (!dir.isEmpty()) edit->setText(dir);
    });
    return edit;
}

// Remembers the typed folder of the category that is being left, then shows the
// folder of the newly picked one.
void SettingsDialog::select_category() {
    if (!category_refreshing_ && !category_dir_edit_->text().trimmed().isEmpty()) {
        category_draft_[current_category()] = category_dir_edit_->text().trimmed().toStdString();
    }
    show_category_dir();
}

void SettingsDialog::show_category_dir() {
    category_refreshing_ = true;
    const std::string category = current_category();
    category_dir_edit_->setText(QString::fromStdString(swiftdm::category_dir_for(settings_, category)));
    category_dir_edit_->setPlaceholderText(default_category_location(category));
    category_refreshing_ = false;
}

// Default destination of the selected category. Shown as placeholder while the
// field is empty, so the folder the download would use stays visible. A stale
// category configuration is not part of the placeholder.
QString SettingsDialog::default_category_location(const std::string& category) const {
    return default_download_location(download_dir_->text().trimmed(),
                                     std::map<std::string, std::string>{},
                                     QString::fromStdString(category),
                                     category_folders_->isChecked());
}

std::string SettingsDialog::current_category() const {
    return category_picker_->currentData().toString().toStdString();
}

QWidget* SettingsDialog::build_general_tab() {
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->setSpacing(kSectionSpacing);

    auto* downloads = add_section(layout, page, tr("Downloads"));
    // Options that only matter while the parent dialog is shown sit indented
    // right below it. A wrapper keeps the indentation independent of the style.
    auto indent = [page](QWidget* child) {
        auto* row = new QWidget(page);
        auto* row_layout = new QHBoxLayout(row);
        row_layout->setContentsMargins(20, 0, 0, 0);
        row_layout->addWidget(child);
        return row;
    };

    show_download_confirmation_ =
        new QCheckBox(tr("Show download confirmation dialog"), page);
    confirm_on_top_ =
        new QCheckBox(tr("Always show the confirmation dialog on top"), page);
    confirm_on_top_->setToolTip(
        tr("Keeps the confirmation above other application windows."));
    // The note explains what happens without the confirmation, so it is only
    // shown while the option is off.
    confirmation_note_ = new QLabel(
        tr("When this option is off, every file is downloaded automatically "
           "without asking for confirmation."),
        page);
    confirmation_note_->setWordWrap(true);
    confirmation_note_->setStyleSheet(QStringLiteral("color:#8a8a8a;"));
    confirmation_note_->setVisible(false);

    completion_dialog_ =
        new QCheckBox(tr("Show download completion dialog"), page);
    completion_on_top_ =
        new QCheckBox(tr("Always show the completion dialog on top"), page);
    completion_on_top_->setToolTip(
        tr("Keeps the completion notification above other application windows."));
    open_folder_after_completion_ =
        new QCheckBox(tr("Open the download folder when a download completes"), page);

    auto sync_sub_options = [this] {
        const bool confirm = show_download_confirmation_->isChecked();
        confirmation_note_->setVisible(!confirm);
        confirm_on_top_->setEnabled(confirm);
        completion_on_top_->setEnabled(completion_dialog_->isChecked());
    };
    connect(show_download_confirmation_, &QCheckBox::toggled, this,
            [sync_sub_options](bool) { sync_sub_options(); });
    connect(completion_dialog_, &QCheckBox::toggled, this,
            [sync_sub_options](bool) { sync_sub_options(); });

    downloads->addWidget(show_download_confirmation_);
    downloads->addWidget(indent(confirm_on_top_));
    downloads->addWidget(confirmation_note_);
    downloads->addWidget(completion_dialog_);
    downloads->addWidget(indent(completion_on_top_));
    downloads->addWidget(open_folder_after_completion_);

    auto* startup = add_section(layout, page, tr("Startup"));
    autostart_ = new QCheckBox(tr("Start application on system startup"), page);
    startup->addWidget(autostart_);

    auto* system_tray = add_section(layout, page, tr("System tray"));
    show_tray_icon_ = new QCheckBox(tr("Show system tray icon"), page);
    close_to_tray_ =
        new QCheckBox(tr("Minimize to tray when closing the window"), page);
    system_tray->addWidget(show_tray_icon_);
    system_tray->addWidget(close_to_tray_);

    layout->addStretch(1);
    return page;
}

QWidget* SettingsDialog::build_interface_tab() {
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->setSpacing(kSectionSpacing);

    auto* toolbar = add_form_section(layout, page, tr("Toolbar"));
    toolbar_size_ = new QComboBox(page);
    toolbar_size_->addItems({tr("Normal"), tr("Small")});
    toolbar->addRow(tr("Toolbar size:"), toolbar_size_);

    show_toolbar_text_ = new QCheckBox(tr("Show toolbar text"), page);
    toolbar->addRow(QString(), show_toolbar_text_);

    auto* appearance = add_form_section(layout, page, tr("Appearance"));
    theme_ = new QComboBox(page);
    theme_->addItems({tr("Light"), tr("Dark"), tr("Follow System")});
    appearance->addRow(tr("App theme:"), theme_);

    auto* system_tray = add_form_section(layout, page, tr("System tray"));
    tray_icon_style_ = new QComboBox(page);
    tray_icon_style_->addItems({tr("Simple"), tr("Colorful")});
    system_tray->addRow(tr("Tray icon style:"), tray_icon_style_);

    layout->addStretch(1);
    return page;
}

QWidget* SettingsDialog::build_download_tab() {
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->setSpacing(kSectionSpacing);

    auto* connections = add_form_section(layout, page, tr("Connections"));
    auto* connections_row = new QHBoxLayout;
    connections_row->setContentsMargins(0, 0, 0, 0);
    max_connections_ = new QSpinBox(page);
    max_connections_->setRange(1, 64);
    connections_row->addWidget(max_connections_);
    auto* recommended = new QLabel(tr("8 connections recommended"), page);
    // Use the theme link color so the hint stays readable on both palettes.
    QPalette hint_palette = recommended->palette();
    hint_palette.setColor(QPalette::WindowText,
                          hint_palette.color(QPalette::Link));
    recommended->setPalette(hint_palette);
    connections_row->addSpacing(12);
    connections_row->addWidget(recommended);
    connections_row->addStretch(1);
    auto* connections_container = new QWidget(page);
    connections_container->setLayout(connections_row);
    connections->addRow(tr("Max connections per download:"), connections_container);

    all_at_once_ = new QRadioButton(tr("All at once"), page);
    one_by_one_ = new QRadioButton(tr("One by one"), page);
    auto* policy = new QWidget(page);
    auto* policy_layout = new QVBoxLayout(policy);
    policy_layout->setContentsMargins(0, 0, 0, 0);
    policy_layout->setSpacing(4);
    policy_layout->addWidget(all_at_once_);
    policy_layout->addWidget(one_by_one_);
    connections->addRow(tr("Create additional connections:"), policy);

    auto* bandwidth = add_form_section(layout, page, tr("Bandwidth"));
    limit_editor_ = new SpeedLimitEditor(page);
    bandwidth->addRow(tr("Bandwidth limit per download:"), limit_editor_);

    auto* user_agent = add_form_section(layout, page, tr("User-Agent"));
    auto* ua_row = new QHBoxLayout;
    ua_row->setContentsMargins(0, 0, 0, 0);
    default_agent_ = new QLineEdit(page);
    auto* reset_ua = new QPushButton(tr("Reset"), page);
    ua_row->addWidget(default_agent_, 1);
    ua_row->addWidget(reset_ua);
    auto* ua_container = new QWidget(page);
    ua_container->setLayout(ua_row);
    user_agent->addRow(tr("Default user-agent:"), ua_container);
    connect(reset_ua, &QPushButton::clicked, this, [this] {
        default_agent_->setText(QString::fromUtf8(swiftdm::kDefaultUserAgent));
    });

    use_ua_browser_ = new QCheckBox(
        tr("Use this UA for both manual and browser-sent downloads (be careful)"),
        page);
    user_agent->addRow(QString(), use_ua_browser_);

    auto* files = add_form_section(layout, page, tr("Files and folders"));
    download_dir_ = add_directory_row(files, page, tr("Download directory:"),
                                      tr("Download Directory"));
    category_folders_ = new QCheckBox(
        tr("Create category folders (e.g. Video, Document, ...)"), page);
    files->addRow(QString(), category_folders_);
    temp_dir_ = add_directory_row(files, page, tr("Temporary directory:"),
                                  tr("Temporary Directory"));
    server_creation_date_ =
        new QCheckBox(tr("Get file creation date from server"), page);
    server_creation_date_->setToolTip(
        tr("Set the downloaded file's modification time from the server date "
           "(HTTP Creation-Date/Last-Modified, FTP MDTM)."));
    files->addRow(QString(), server_creation_date_);
    auto* category_dirs = add_form_section(layout, page, tr("Category folders"));
    category_picker_ = new QComboBox(page);
    for (const std::string& category : category_names()) {
        category_picker_->addItem(QString::fromStdString(category),
                                  QString::fromStdString(category));
    }
    category_dirs->addRow(tr("Category:"), category_picker_);
    category_dir_edit_ = add_directory_row(category_dirs, page, tr("Folder:"),
                                           tr("Category Folder"));
    auto* category_note = new QLabel(
        tr("Leave the folder empty to use the default location. A "
           "custom folder replaces the category subfolder and is "
           "used even when category folders are off."),
        page);
    category_note->setWordWrap(true);
    QPalette note_palette = category_note->palette();
    note_palette.setColor(QPalette::WindowText, note_palette.color(QPalette::Link));
    category_note->setPalette(note_palette);
    category_dirs->addRow(category_note);
    auto* reset = new QPushButton(tr("Reset"), category_dir_edit_->parentWidget());
    static_cast<QHBoxLayout*>(category_dir_edit_->parentWidget()->layout())
        ->addWidget(reset);
    connect(reset, &QPushButton::clicked, category_dir_edit_, &QLineEdit::clear);
    connect(category_dir_edit_, &QLineEdit::textEdited, this, [this] { show_category_dir(); });
    connect(category_picker_, &QComboBox::currentIndexChanged, this,
            [this] { select_category(); });
    // Category folders off means the default location has no subfolder either.
    connect(category_folders_, &QCheckBox::toggled, this, [this] { show_category_dir(); });

    layout->addStretch(1);
    return page;
}

QWidget* SettingsDialog::build_proxy_tab() {
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->setSpacing(kSectionSpacing);

    auto* proxy_type = add_section(layout, page, tr("Proxy type"));
    proxy_none_ = new QRadioButton(tr("No Proxy/Socks"), page);
    proxy_http_ = new QRadioButton(tr("HTTP Proxy"), page);
    proxy_type->addWidget(proxy_none_);
    proxy_type->addWidget(proxy_http_);

    auto* socks_row = new QHBoxLayout;
    socks_row->setContentsMargins(0, 0, 0, 0);
    proxy_socks_ = new QRadioButton(tr("Socks Proxy"), page);
    socks_row->addWidget(proxy_socks_);
    socks_version_ = new QComboBox(page);
    socks_version_->addItems({tr("V4"), tr("V5")});
    socks_row->addWidget(socks_version_);
    socks_row->addStretch(1);
    proxy_type->addLayout(socks_row);

    auto* note = new QLabel(
        tr("For https addresses the proxy handles the CONNECT tunnel."), page);
    note->setWordWrap(true);
    proxy_type->addWidget(note);

    auto* server = add_form_section(layout, page, tr("Server"));
    proxy_address_ = new QLineEdit(page);
    proxy_port_ = new QSpinBox(page);
    proxy_port_->setRange(0, 65535);
    server->addRow(tr("Address:"), proxy_address_);
    server->addRow(tr("Port:"), proxy_port_);

    auto* authentication = add_form_section(layout, page, tr("Authentication"));
    proxy_user_ = new QLineEdit(page);
    proxy_password_ = new QLineEdit(page);
    proxy_password_->setEchoMode(QLineEdit::Password);
    authentication->addRow(tr("User:"), proxy_user_);
    authentication->addRow(tr("Password:"), proxy_password_);

    layout->addStretch(1);

    connect(proxy_none_, &QRadioButton::toggled, this, [this] { update_proxy_enabled(); });
    connect(proxy_http_, &QRadioButton::toggled, this, [this] { update_proxy_enabled(); });
    connect(proxy_socks_, &QRadioButton::toggled, this, [this] { update_proxy_enabled(); });
    return page;
}

void SettingsDialog::update_proxy_enabled() {
    const bool http = proxy_http_->isChecked();
    const bool socks = proxy_socks_->isChecked();
    const bool enabled = http || socks;
    proxy_address_->setEnabled(enabled);
    proxy_port_->setEnabled(enabled);
    proxy_user_->setEnabled(enabled);
    proxy_password_->setEnabled(enabled);
    socks_version_->setEnabled(socks);
}

void SettingsDialog::load_from_settings() {
    show_download_confirmation_->setChecked(settings_.show_download_confirmation);
    // setChecked() only emits toggled() when the state changes, so the note is
    // updated explicitly for the stored value.
    confirmation_note_->setVisible(!settings_.show_download_confirmation);
    confirm_on_top_->setChecked(settings_.confirm_dialog_on_top);
    confirm_on_top_->setEnabled(settings_.show_download_confirmation);
    completion_dialog_->setChecked(settings_.completion_dialog);
    completion_on_top_->setChecked(settings_.completion_dialog_on_top);
    completion_on_top_->setEnabled(settings_.completion_dialog);
    autostart_->setChecked(settings_.autostart);
    show_tray_icon_->setChecked(settings_.show_tray_icon);
    close_to_tray_->setChecked(settings_.close_to_tray);
    open_folder_after_completion_->setChecked(settings_.open_folder_after_completion);

    toolbar_size_->setCurrentIndex(settings_.toolbar_size == "small" ? 1 : 0);
    show_toolbar_text_->setChecked(settings_.show_toolbar_text);
    if (settings_.theme == "light") {
        theme_->setCurrentIndex(0);
    } else if (settings_.theme == "system") {
        theme_->setCurrentIndex(2);
    } else {
        theme_->setCurrentIndex(1);
    }
    tray_icon_style_->setCurrentIndex(settings_.tray_icon_style == "simple" ? 0 : 1);

    max_connections_->setValue(settings_.max_connections);
    if (settings_.connections_at_once != 0) {
        one_by_one_->setChecked(true);
    } else {
        all_at_once_->setChecked(true);
    }
    limit_editor_->set_value(settings_.bandwidth_limit_enabled,
                             settings_.bandwidth_limit_kbps);
    default_agent_->setText(QString::fromStdString(settings_.default_agent));
    use_ua_browser_->setChecked(settings_.use_ua_for_browser);
    download_dir_->setText(QString::fromStdString(settings_.download_dir));
    connect(download_dir_, &QLineEdit::textChanged, this, [this] { show_category_dir(); });
    category_folders_->setChecked(settings_.category_folders);
    category_draft_ = settings_.category_dirs;
    show_category_dir();
    temp_dir_->setText(QString::fromStdString(settings_.temp_dir));
    server_creation_date_->setChecked(settings_.server_creation_date);

    if (settings_.proxy.type == "http") {
        proxy_http_->setChecked(true);
    } else if (settings_.proxy.type == "socks4" || settings_.proxy.type == "socks5") {
        proxy_socks_->setChecked(true);
        socks_version_->setCurrentIndex(settings_.proxy.type == "socks4" ? 0 : 1);
    } else {
        proxy_none_->setChecked(true);
    }
    proxy_address_->setText(QString::fromStdString(settings_.proxy.address));
    proxy_port_->setValue(settings_.proxy.port);
    proxy_user_->setText(QString::fromStdString(settings_.proxy.user));
    proxy_password_->setText(QString::fromStdString(settings_.proxy.password));
    update_proxy_enabled();
}

void SettingsDialog::store_to_settings() {
    settings_.show_download_confirmation = show_download_confirmation_->isChecked();
    settings_.confirm_dialog_on_top = confirm_on_top_->isChecked();
    settings_.completion_dialog = completion_dialog_->isChecked();
    settings_.completion_dialog_on_top = completion_on_top_->isChecked();
    settings_.autostart = autostart_->isChecked();
    settings_.show_tray_icon = show_tray_icon_->isChecked();
    settings_.close_to_tray = close_to_tray_->isChecked();
    settings_.open_folder_after_completion =
        open_folder_after_completion_->isChecked();

    settings_.toolbar_size = toolbar_size_->currentIndex() == 1 ? "small" : "normal";
    settings_.show_toolbar_text = show_toolbar_text_->isChecked();
    if (theme_->currentIndex() == 0) {
        settings_.theme = "light";
    } else if (theme_->currentIndex() == 2) {
        settings_.theme = "system";
    } else {
        settings_.theme = "dark";
    }
    settings_.tray_icon_style =
        tray_icon_style_->currentIndex() == 0 ? "simple" : "colorful";

    settings_.max_connections = max_connections_->value();
    settings_.connections_at_once = one_by_one_->isChecked() ? 1 : 0;
    settings_.bandwidth_limit_enabled = limit_editor_->enabled();
    settings_.bandwidth_limit_kbps = limit_editor_->kbps();
    settings_.default_agent = default_agent_->text().trimmed().toStdString();
    if (settings_.default_agent.empty()) {
        settings_.default_agent = swiftdm::kDefaultUserAgent;
    }
    settings_.use_ua_for_browser = use_ua_browser_->isChecked();
    settings_.download_dir =
        swiftdm::expand_tilde(download_dir_->text().trimmed().toStdString());
    settings_.category_folders = category_folders_->isChecked();
    settings_.category_dirs.clear();
    for (const auto& [category, dir] : category_draft_) {
        if (!dir.empty()) settings_.category_dirs[category] = dir;
    }
    // The visible row may still hold an edit that was never committed by
    // switching category.
    const QString current_dir = category_dir_edit_->text().trimmed();
    if (!current_dir.isEmpty()) {
        settings_.category_dirs[current_category()] = current_dir.toStdString();
    }
    settings_.temp_dir = swiftdm::expand_tilde(temp_dir_->text().trimmed().toStdString());
    settings_.server_creation_date = server_creation_date_->isChecked();

    if (proxy_http_->isChecked()) {
        settings_.proxy.type = "http";
    } else if (proxy_socks_->isChecked()) {
        settings_.proxy.type = socks_version_->currentIndex() == 0 ? "socks4" : "socks5";
    } else {
        settings_.proxy.type = "none";
    }
    settings_.proxy.address = proxy_address_->text().trimmed().toStdString();
    settings_.proxy.port = proxy_port_->value();
    settings_.proxy.user = proxy_user_->text().toStdString();
    settings_.proxy.password = proxy_password_->text().toStdString();
}

}  // namespace swiftdm::ui
