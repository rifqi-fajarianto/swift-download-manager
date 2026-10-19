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
    // [general]
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
    bool completion_dialog = true;  // CompletionDialog
    bool autostart = false;         // AppAutoStart

    // [proxy]
    ProxySettings proxy;

    // Location this settings object was loaded from / saved to.
    std::string config_path;
};

// Default User-Agent used when the setting is empty. Linux desktop Chrome UA.
extern const char* const kDefaultUserAgent;

std::string default_config_path();  // $XDG_CONFIG_HOME/swiftdm/swiftdm.ini
std::string default_temp_dir();     // $XDG_DATA_HOME/swiftdm/tmp
std::string default_database_path();  // $XDG_DATA_HOME/swiftdm/swiftdm.db

Settings default_settings();

// Loads settings; missing file yields defaults (not an error). Unparseable
// lines are skipped so a partially corrupted file still loads.
bool load_settings(const std::string& path, Settings& out, std::string* error = nullptr);

// Saves settings; creates parent directories as needed.
bool save_settings(const Settings& settings, std::string* error = nullptr);

// Writes/removes the XDG autostart entry (~/.config/autostart/swiftdm.desktop)
// according to the AppAutoStart setting.
bool apply_autostart(bool enabled, std::string* error = nullptr);

}  // namespace swiftdm
