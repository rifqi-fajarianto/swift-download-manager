#pragma once

#include <QDialog>

#include "store/settings.h"
#include "ui/speed_limit_editor.h"

class QCheckBox;
class QComboBox;
class QFormLayout;
class QLabel;
class QLineEdit;
class QRadioButton;
class QSpinBox;

namespace swiftdm::ui {

// Settings dialog with tabs General / Interface / Download / Proxy-Socks /
// Passwords, mirroring the reference options window
// (docs/spec/01-product.md §3.7, 05-settings.md).
class SettingsDialog : public QDialog {
    Q_OBJECT
  public:
    SettingsDialog(swiftdm::Settings& settings, QWidget* parent = nullptr);

  private:
    void load_from_settings();
    void store_to_settings();

    QWidget* build_general_tab();
    QWidget* build_interface_tab();
    QWidget* build_download_tab();
    QWidget* build_proxy_tab();
    QWidget* build_passwords_tab();

    // Adds a directory picker row to `layout`; returns the line edit. The row
    // widgets are parented to `parent` so they stay inside the owning tab page.
    QLineEdit* add_directory_row(QFormLayout* layout, QWidget* parent,
                                 const QString& title);

    swiftdm::Settings& settings_;

    // General tab
    QCheckBox* completion_dialog_ = nullptr;
    QCheckBox* autostart_ = nullptr;

    // Interface tab
    QLabel* interface_placeholder_ = nullptr;

    // Download tab
    QSpinBox* max_connections_ = nullptr;
    QRadioButton* all_at_once_ = nullptr;
    QRadioButton* one_by_one_ = nullptr;
    SpeedLimitEditor* limit_editor_ = nullptr;
    QLineEdit* default_agent_ = nullptr;
    QCheckBox* use_ua_browser_ = nullptr;
    QLineEdit* download_dir_ = nullptr;
    QCheckBox* category_folders_ = nullptr;
    QLineEdit* temp_dir_ = nullptr;
    QCheckBox* server_creation_date_ = nullptr;

    // Proxy tab
    QRadioButton* proxy_none_ = nullptr;
    QRadioButton* proxy_http_ = nullptr;
    QRadioButton* proxy_socks_ = nullptr;
    QComboBox* socks_version_ = nullptr;
    QLineEdit* proxy_address_ = nullptr;
    QSpinBox* proxy_port_ = nullptr;
    QLineEdit* proxy_user_ = nullptr;
    QLineEdit* proxy_password_ = nullptr;

    void update_proxy_enabled();
};

}  // namespace swiftdm::ui
