#include "ui/settings_dialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
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
#include "swiftdm/version.h"

namespace swiftdm::ui {

SettingsDialog::SettingsDialog(swiftdm::Settings& settings, QWidget* parent)
    : QDialog(parent), settings_(settings) {
    setWindowTitle(tr("SwiftDM Settings"));
    resize(640, 540);

    auto* tabs = new QTabWidget(this);
    tabs->addTab(build_general_tab(), tr("General"));
    tabs->addTab(build_interface_tab(), tr("Interface"));
    tabs->addTab(build_download_tab(), tr("Download"));
    tabs->addTab(build_proxy_tab(), tr("Proxy/Socks"));
    tabs->addTab(build_passwords_tab(), tr("Passwords"));

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        store_to_settings();
        std::string error;
        if (!swiftdm::save_settings(settings_, &error)) {
            QMessageBox::warning(this, tr("Settings"),
                                 tr("Could not save settings: %1")
                                     .arg(QString::fromStdString(error)));
        }
        std::string autostart_error;
        if (!swiftdm::apply_autostart(settings_.autostart, &autostart_error)) {
            QMessageBox::warning(this, tr("Settings"),
                                 tr("Could not update autostart entry: %1")
                                     .arg(QString::fromStdString(autostart_error)));
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
// widgets are parented to `parent` so they stay inside the tab page that owns
// the form layout.
QLineEdit* SettingsDialog::add_directory_row(QFormLayout* layout, QWidget* parent,
                                             const QString& title) {
    auto* container = new QWidget(parent);
    auto* row = new QHBoxLayout(container);
    auto* edit = new QLineEdit(container);
    auto* browse = new QPushButton("...", container);
    browse->setFixedWidth(32);
    row->addWidget(edit, 1);
    row->addWidget(browse);
    layout->addRow(title, container);
    connect(browse, &QPushButton::clicked, this, [this, edit, title] {
        const QString start =
            QString::fromStdString(swiftdm::expand_tilde(edit->text().toStdString()));
        const QString dir = QFileDialog::getExistingDirectory(this, title, start);
        if (!dir.isEmpty()) edit->setText(dir);
    });
    return edit;
}

QWidget* SettingsDialog::build_general_tab() {
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);

    completion_dialog_ = new QCheckBox(tr("Show Download Completion Dialog"), page);
    autostart_ = new QCheckBox(tr("Start Application on System Startup"), page);
    layout->addWidget(completion_dialog_);
    layout->addWidget(autostart_);
    layout->addStretch(1);
    return page;
}

QWidget* SettingsDialog::build_interface_tab() {
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    // The appearance options (toolbar size, toolbar text, theme, tray icon
    // style) arrive with a later patch release of this batch.
    interface_placeholder_ = new QLabel(
        tr("The appearance options (toolbar size, toolbar text, theme and tray\n"
           "icon style) are added in a later release of this batch."),
        page);
    interface_placeholder_->setWordWrap(true);
    layout->addWidget(interface_placeholder_);
    layout->addStretch(1);
    return page;
}

QWidget* SettingsDialog::build_download_tab() {
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);

    // The sub-forms are plain layouts: they are installed into the page layout
    // below, so they must not take over the page layout themselves.
    auto* form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignLeft);
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    layout->addLayout(form);

    auto* connections_row = new QHBoxLayout;
    max_connections_ = new QSpinBox(page);
    max_connections_->setRange(1, 64);
    connections_row->addWidget(max_connections_);
    auto* recommended = new QLabel(tr("8 Connections Recommended"), page);
    recommended->setStyleSheet("color:#4da3ff;");
    connections_row->addSpacing(12);
    connections_row->addWidget(recommended);
    connections_row->addStretch(1);
    auto* connections_container = new QWidget(page);
    connections_container->setLayout(connections_row);
    form->addRow(tr("Max Connections per Download"), connections_container);

    layout->addSpacing(6);

    layout->addWidget(new QLabel(tr("When first Connection Starts Downloading , : "), page));
    all_at_once_ = new QRadioButton(tr("Create Additional Connections All at Once"), page);
    one_by_one_ = new QRadioButton(tr("Create Additional Connections One by One"), page);
    layout->addWidget(all_at_once_);
    layout->addWidget(one_by_one_);
    layout->addSpacing(6);

    auto* form2 = new QFormLayout;
    form2->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    layout->addLayout(form2);

    limit_editor_ = new SpeedLimitEditor(page);
    form2->addRow(tr("Bandwidth Limit per Download"), limit_editor_);

    auto* ua_row = new QHBoxLayout;
    default_agent_ = new QLineEdit(page);
    auto* reset_ua = new QPushButton(tr("Reset"), page);
    ua_row->addWidget(default_agent_, 1);
    ua_row->addWidget(reset_ua);
    auto* ua_container = new QWidget(page);
    ua_container->setLayout(ua_row);
    form2->addRow(tr("Default User-Agent"), ua_container);
    connect(reset_ua, &QPushButton::clicked, this, [this] {
        default_agent_->setText(QString::fromUtf8(swiftdm::kDefaultUserAgent));
    });

    use_ua_browser_ = new QCheckBox(
        tr("Use this UA for both Manual and Browser-Sent Downloads ( Be Careful )"), page);
    form2->addRow(QString(), use_ua_browser_);

    download_dir_ = add_directory_row(form2, page, tr("Download Directory"));
    category_folders_ =
        new QCheckBox(tr("Create Category Folders ( e.g. Video, Document, ... )"), page);
    form2->addRow(QString(), category_folders_);

    temp_dir_ = add_directory_row(form2, page, tr("Temporary Directory"));
    server_creation_date_ = new QCheckBox(tr("Get File Creation Date From Server"), page);
    server_creation_date_->setToolTip(
        tr("Belum diimplementasikan di SwiftDM (disimpan sebagai setelan)."));
    form2->addRow(QString(), server_creation_date_);

    layout->addStretch(1);
    return page;
}

QWidget* SettingsDialog::build_proxy_tab() {
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);

    proxy_none_ = new QRadioButton(tr("No Proxy/Socks"), page);
    proxy_http_ = new QRadioButton(tr("HTTP Proxy"), page);
    proxy_socks_ = new QRadioButton(tr("Socks Proxy"), page);
    layout->addWidget(proxy_none_);
    layout->addWidget(proxy_http_);

    auto* socks_row = new QHBoxLayout;
    socks_row->addWidget(proxy_socks_);
    socks_version_ = new QComboBox(page);
    socks_version_->addItems({tr("V4"), tr("V5")});
    socks_row->addWidget(socks_version_);
    socks_row->addStretch(1);
    layout->addLayout(socks_row);
    layout->addSpacing(8);

    auto* form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    layout->addLayout(form);

    proxy_address_ = new QLineEdit(page);
    proxy_port_ = new QSpinBox(page);
    proxy_port_->setRange(0, 65535);
    proxy_user_ = new QLineEdit(page);
    proxy_password_ = new QLineEdit(page);
    proxy_password_->setEchoMode(QLineEdit::Password);
    form->addRow(tr("Address"), proxy_address_);
    form->addRow(tr("Port"), proxy_port_);
    form->addRow(tr("User"), proxy_user_);
    form->addRow(tr("Password"), proxy_password_);

    layout->addSpacing(8);
    auto* note = new QLabel(
        tr("For https addresses the proxy handles the CONNECT tunnel."), page);
    note->setWordWrap(true);
    layout->addWidget(note);
    layout->addStretch(1);

    connect(proxy_none_, &QRadioButton::toggled, this, [this] { update_proxy_enabled(); });
    connect(proxy_http_, &QRadioButton::toggled, this, [this] { update_proxy_enabled(); });
    connect(proxy_socks_, &QRadioButton::toggled, this, [this] { update_proxy_enabled(); });
    return page;
}

QWidget* SettingsDialog::build_passwords_tab() {
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    auto* note = new QLabel(
        tr("Saved server credentials (Basic/Digest) will live here in a later\n"
           "milestone; the storage layout is specified in docs/spec/02-data-model.md\n"
           "(auths table)."),
        page);
    note->setWordWrap(true);
    layout->addWidget(note);
    layout->addStretch(1);
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
    completion_dialog_->setChecked(settings_.completion_dialog);
    autostart_->setChecked(settings_.autostart);

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
    category_folders_->setChecked(settings_.category_folders);
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
    settings_.completion_dialog = completion_dialog_->isChecked();
    settings_.autostart = autostart_->isChecked();

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
