#pragma once

#include <string>

namespace swiftdm {

// Proxy configuration (spec 05-settings.md). `type` is one of:
// "none", "http", "socks4", "socks5".
struct ProxySettings {
    std::string type = "none";
    std::string address;
    int port = 0;
    std::string user;
    std::string password;

    bool enabled() const { return type != "none" && !address.empty() && port > 0; }
};

// Application settings. Key names follow the original registry values
// (evidence ledger E8) so the mapping stays traceable.
struct Settings {
    // [general] — app-wide behavior.
    bool completion_dialog = true;  // CompletionDialog
    bool autostart = false;         // AppAutoStart
    bool show_tray_icon = true;     // ShowTrayIcon
    bool close_to_tray = true;      // CloseToTray
    bool open_folder_after_completion = false;  // OpenFolderAfterCompletion
    // Ask before every transfer starts. When it is off the download starts
    // right away with the default category and folder.
    bool show_download_confirmation = true;  // ShowDownloadConfirmationDialog

    // [download] — download defaults.
    int max_connections = 8;        // MaxConnections, default 8 ("8 recommended")
    int connections_at_once = 0;    // ConnectionsAtOnce: 0 = all at once, 1 = one by one
    // BandWidthLimit + BandWidthLimitEnabled: the toggle decides whether the
    // limiter runs; the value is kept while the limiter is off so the user can
    // prepare it. A legacy config with a non-zero BandWidthLimit but no toggle
    // key loads as enabled.
    int bandwidth_limit_kbps = 1024;  // BandWidthLimit, default 1 MB/s
    bool bandwidth_limit_enabled = false;  // BandWidthLimitEnabled
    bool use_ua_for_browser = false;  // UseUAgent
    std::string default_agent;      // DefaultAgent (filled by default_settings())
    std::string download_dir;       // DownloadDirectory
    bool category_folders = true;   // CategoryFolders
    std::string temp_dir;           // TempDirectory
    bool server_creation_date = false;  // ServerCreationDate

    // [interface] — appearance. The allowed values are checked by the
    // is_valid_* helpers below; an unknown value keeps the default.
    std::string theme = "dark";              // Theme: "system" | "light" | "dark"
    std::string toolbar_size = "normal";     // ToolbarSize: "normal" | "small"
    bool show_toolbar_text = true;           // ShowToolbarText
    std::string tray_icon_style = "colorful";  // TrayIconStyle: "colorful" | "simple"
    // Main window geometry, written when the window is resized or moved. The
    // position is only restored when both coordinates were saved; otherwise
    // the window manager places the window.
    int main_window_width = 1020;   // WindowWidth
    int main_window_height = 580;   // WindowHeight
    int main_window_x = 0;          // WindowX
    int main_window_y = 0;          // WindowY
    bool has_main_window_position = false;  // WindowX and WindowY both present

    // [proxy]
    ProxySettings proxy;

    // Location this settings object was loaded from / saved to.
    std::string config_path;
};

// Default User-Agent used when the setting is empty. Linux desktop Chrome UA.
extern const char* const kDefaultUserAgent;

// Allowed values for the interface keys.
bool is_valid_theme(const std::string& value);            // system | light | dark
bool is_valid_toolbar_size(const std::string& value);     // normal | small
bool is_valid_tray_icon_style(const std::string& value);  // colorful | simple

std::string default_config_path();  // platform config root/SwiftDM/swiftdm.ini
std::string default_temp_dir();     // platform local data root/SwiftDM/tmp
std::string default_database_path();  // platform local data root/SwiftDM/swiftdm.db
std::string default_log_dir();  // platform local data root/SwiftDM/logs

Settings default_settings();

// Loads settings; missing file yields defaults (not an error). Unparseable
// lines are skipped so a partially corrupted file still loads.
bool load_settings(const std::string& path, Settings& out, std::string* error = nullptr);

// Saves settings; creates parent directories as needed.
bool save_settings(const Settings& settings, std::string* error = nullptr);

// Writes/removes the platform login-start entry according to AppAutoStart.
bool apply_autostart(bool enabled, std::string* error = nullptr);

}  // namespace swiftdm
