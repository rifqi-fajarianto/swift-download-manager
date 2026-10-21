#include "store/settings.h"

#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <map>
#include <string>

#include "core/util.h"

namespace swiftdm {

// Behavior of the original default UA (Windows Chrome 107) adapted to Linux;
// the exact original string is recorded in the evidence ledger (E24).
const char* const kDefaultUserAgent =
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/107.0.0.0 Safari/537.36";

namespace {

bool parse_bool(const std::string& v, bool fallback) {
    const std::string s = to_lower(trim(v));
    if (s == "1" || s == "true" || s == "yes" || s == "on") return true;
    if (s == "0" || s == "false" || s == "no" || s == "off") return false;
    return fallback;
}

bool parse_int(const std::string& v, int fallback, int& out) {
    std::string s = trim(v);
    if (s.empty()) return false;
    char* end = nullptr;
    const long value = std::strtol(s.c_str(), &end, 10);
    if (end == s.c_str() || (end && *end != '\0')) return false;
    out = static_cast<int>(value);
    return true;
}

std::string proxy_type_to_token(const std::string& type) {
    if (type == "http") return "http";
    if (type == "socks4") return "4";
    if (type == "socks5") return "5";
    return "0";
}

std::string proxy_token_to_type(const std::string& token) {
    const std::string t = to_lower(trim(token));
    if (t == "http") return "http";
    if (t == "4" || t == "socks4") return "socks4";
    if (t == "5" || t == "socks5") return "socks5";
    return "none";
}

// Returns the value when it is one of the allowed choices, otherwise the
// fallback. The comparison ignores case and surrounding spaces.
std::string pick_choice(const std::string& value,
                        std::initializer_list<const char*> choices,
                        const std::string& fallback) {
    const std::string v = to_lower(trim(value));
    for (const char* choice : choices) {
        if (v == choice) return v;
    }
    return fallback;
}

// Applies one section of the configuration file. The keys are already lower
// case. Unknown keys and unknown sections are ignored, so a config written by
// a newer release still loads.
void apply_section(Settings& s, const std::string& section,
                   const std::map<std::string, std::string>& keys,
                   bool* saw_bandwidth_toggle) {
    for (const auto& entry : keys) {
        const std::string& k = entry.first;
        const std::string& value = entry.second;
        if (section == "general") {
            if (k == "completiondialog") {
                s.completion_dialog = parse_bool(value, s.completion_dialog);
            } else if (k == "appautostart") {
                s.autostart = parse_bool(value, s.autostart);
            } else if (k == "showtrayicon") {
                s.show_tray_icon = parse_bool(value, s.show_tray_icon);
            } else if (k == "closetotray") {
                s.close_to_tray = parse_bool(value, s.close_to_tray);
            } else if (k == "openfolderaftercompletion") {
                s.open_folder_after_completion =
                    parse_bool(value, s.open_folder_after_completion);
            } else if (k == "showdownloadconfirmationdialog") {
                s.show_download_confirmation =
                    parse_bool(value, s.show_download_confirmation);
            }
            // Legacy location of the download options. Configs written before
            // the [download] section existed keep working; the dedicated
            // section is applied afterwards and wins.
            else if (k == "maxconnections") parse_int(value, s.max_connections, s.max_connections);
            else if (k == "connectionsatonce") parse_int(value, s.connections_at_once, s.connections_at_once);
            else if (k == "bandwidthlimit") parse_int(value, s.bandwidth_limit_kbps, s.bandwidth_limit_kbps);
            else if (k == "bandwidthlimitenabled") {
                if (saw_bandwidth_toggle) *saw_bandwidth_toggle = true;
                s.bandwidth_limit_enabled =
                    parse_bool(value, s.bandwidth_limit_enabled);
            }
            else if (k == "useuagent") s.use_ua_for_browser = parse_bool(value, s.use_ua_for_browser);
            else if (k == "defaultagent") s.default_agent = value;
            else if (k == "downloaddirectory") s.download_dir = expand_tilde(value);
            else if (k == "categoryfolders") s.category_folders = parse_bool(value, s.category_folders);
            else if (k == "tempdirectory") s.temp_dir = expand_tilde(value);
            else if (k == "servercreationdate") s.server_creation_date = parse_bool(value, s.server_creation_date);
        } else if (section == "download") {
            if (k == "maxconnections") parse_int(value, s.max_connections, s.max_connections);
            else if (k == "connectionsatonce") parse_int(value, s.connections_at_once, s.connections_at_once);
            else if (k == "bandwidthlimit") parse_int(value, s.bandwidth_limit_kbps, s.bandwidth_limit_kbps);
            else if (k == "bandwidthlimitenabled") {
                if (saw_bandwidth_toggle) *saw_bandwidth_toggle = true;
                s.bandwidth_limit_enabled =
                    parse_bool(value, s.bandwidth_limit_enabled);
            }
            else if (k == "useuagent") s.use_ua_for_browser = parse_bool(value, s.use_ua_for_browser);
            else if (k == "defaultagent") s.default_agent = value;
            else if (k == "downloaddirectory") s.download_dir = expand_tilde(value);
            else if (k == "categoryfolders") s.category_folders = parse_bool(value, s.category_folders);
            else if (k == "tempdirectory") s.temp_dir = expand_tilde(value);
            else if (k == "servercreationdate") s.server_creation_date = parse_bool(value, s.server_creation_date);
        } else if (section == "interface") {
            if (k == "theme") {
                s.theme = pick_choice(value, {"system", "light", "dark"}, s.theme);
            } else if (k == "toolbarsize") {
                s.toolbar_size = pick_choice(value, {"normal", "small"}, s.toolbar_size);
            } else if (k == "showtoolbartext") {
                s.show_toolbar_text = parse_bool(value, s.show_toolbar_text);
            } else if (k == "trayiconstyle") {
                s.tray_icon_style =
                    pick_choice(value, {"colorful", "simple"}, s.tray_icon_style);
            }
        } else if (section == "proxy") {
            if (k == "socksversion") s.proxy.type = proxy_token_to_type(value);
            else if (k == "proxyaddress") s.proxy.address = trim(value);
            else if (k == "proxyport") parse_int(value, s.proxy.port, s.proxy.port);
            else if (k == "proxyuser") s.proxy.user = value;
            else if (k == "proxypassword") s.proxy.password = value;
        }
    }
}

}  // namespace

std::string default_config_path() { return xdg_config_home() + "/swiftdm/swiftdm.ini"; }

std::string default_temp_dir() { return xdg_data_home() + "/swiftdm/tmp"; }

std::string default_database_path() { return xdg_data_home() + "/swiftdm/swiftdm.db"; }

bool is_valid_theme(const std::string& value) {
    const std::string v = to_lower(trim(value));
    return v == "system" || v == "light" || v == "dark";
}

bool is_valid_toolbar_size(const std::string& value) {
    const std::string v = to_lower(trim(value));
    return v == "normal" || v == "small";
}

bool is_valid_tray_icon_style(const std::string& value) {
    const std::string v = to_lower(trim(value));
    return v == "colorful" || v == "simple";
}

Settings default_settings() {
    Settings s;
    s.default_agent = kDefaultUserAgent;
    s.download_dir = home_dir() + "/Downloads";
    s.temp_dir = default_temp_dir();
    s.config_path = default_config_path();
    return s;
}

bool load_settings(const std::string& path, Settings& out, std::string* error) {
    out = default_settings();
    out.config_path = path;

    std::ifstream in(path);
    if (!in) {
        // Missing configuration is normal: defaults apply.
        return true;
    }

    // The file is read into sections first, so the order of the sections in
    // the file cannot change the outcome. Keys are stored lower case.
    std::map<std::string, std::map<std::string, std::string>> sections;
    std::string section = "general";
    std::string line;
    while (std::getline(in, line)) {
        std::string t = trim(line);
        if (t.empty() || t[0] == ';' || t[0] == '#') continue;
        if (t.front() == '[' && t.back() == ']') {
            section = to_lower(trim(t.substr(1, t.size() - 2)));
            continue;
        }
        const auto eq = t.find('=');
        if (eq == std::string::npos) continue;  // skip unparseable line
        const std::string key = trim(t.substr(0, eq));
        const std::string value = trim(t.substr(eq + 1));
        if (key.empty()) continue;
        sections[section][to_lower(key)] = value;
    }

    // [general] is applied first because it still holds the download options
    // of older configs; the dedicated [download] section overrides them.
    bool saw_bandwidth_toggle = false;
    for (const char* name : {"general", "download", "interface", "proxy"}) {
        const auto found = sections.find(name);
        if (found == sections.end()) continue;
        apply_section(out, name, found->second, &saw_bandwidth_toggle);
    }

    // Configs written before the toggle existed used any non-zero
    // BandWidthLimit as "limiter on"; keep that behavior on upgrade.
    if (!saw_bandwidth_toggle && out.bandwidth_limit_kbps > 0) {
        out.bandwidth_limit_enabled = true;
    }

    if (error) error->clear();
    return true;
}

bool save_settings(const Settings& settings, std::string* error) {
    const std::string path = settings.config_path;
    std::error_code ec;
    if (!path.empty()) {
        std::filesystem::create_directories(
            std::filesystem::path(path).parent_path(), ec);
        if (ec) {
            if (error) *error = "cannot create config directory: " + ec.message();
            return false;
        }
    }

    std::ofstream out(path, std::ios::trunc);
    if (!out) {
        if (error) *error = "cannot open '" + path + "' for writing";
        return false;
    }

    out << "# SwiftDM configuration (see docs/spec/05-settings.md)\n"
        << "[general]\n"
        << "CompletionDialog = " << (settings.completion_dialog ? 1 : 0) << "\n"
        << "AppAutoStart = " << (settings.autostart ? 1 : 0) << "\n"
        << "ShowTrayIcon = " << (settings.show_tray_icon ? 1 : 0) << "\n"
        << "CloseToTray = " << (settings.close_to_tray ? 1 : 0) << "\n"
        << "OpenFolderAfterCompletion = "
        << (settings.open_folder_after_completion ? 1 : 0) << "\n"
        << "ShowDownloadConfirmationDialog = "
        << (settings.show_download_confirmation ? 1 : 0) << "\n"
        << "\n[download]\n"
        << "MaxConnections = " << settings.max_connections << "\n"
        << "ConnectionsAtOnce = " << settings.connections_at_once << "\n"
        << "BandWidthLimit = " << settings.bandwidth_limit_kbps << "\n"
        << "BandWidthLimitEnabled = " << (settings.bandwidth_limit_enabled ? 1 : 0) << "\n"
        << "UseUAgent = " << (settings.use_ua_for_browser ? 1 : 0) << "\n"
        << "DefaultAgent = " << settings.default_agent << "\n"
        << "DownloadDirectory = " << settings.download_dir << "\n"
        << "CategoryFolders = " << (settings.category_folders ? 1 : 0) << "\n"
        << "TempDirectory = " << settings.temp_dir << "\n"
        << "ServerCreationDate = " << (settings.server_creation_date ? 1 : 0) << "\n"
        << "\n[interface]\n"
        << "Theme = " << settings.theme << "\n"
        << "ToolbarSize = " << settings.toolbar_size << "\n"
        << "ShowToolbarText = " << (settings.show_toolbar_text ? 1 : 0) << "\n"
        << "TrayIconStyle = " << settings.tray_icon_style << "\n"
        << "\n[proxy]\n"
        << "SocksVersion = " << proxy_type_to_token(settings.proxy.type) << "\n"
        << "ProxyAddress = " << settings.proxy.address << "\n"
        << "ProxyPort = " << settings.proxy.port << "\n"
        << "ProxyUser = " << settings.proxy.user << "\n"
        << "ProxyPassword = " << settings.proxy.password << "\n";

    out.flush();
    if (!out) {
        if (error) *error = "write failed for '" + path + "'";
        return false;
    }
    if (error) error->clear();
    return true;
}

bool apply_autostart(bool enabled, std::string* error) {
    const std::string dir = xdg_config_home() + "/autostart";
    const std::string path = dir + "/swiftdm.desktop";

    std::error_code ec;
    if (!enabled) {
        std::filesystem::remove(path, ec);
        if (error) error->clear();
        return true;
    }

    std::filesystem::create_directories(dir, ec);
    if (ec) {
        if (error) *error = "cannot create autostart directory: " + ec.message();
        return false;
    }

    std::string exe = "swiftdm";
    char buffer[4096];
    const ssize_t length = ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (length > 0) {
        buffer[length] = '\0';
        exe = buffer;
    }

    std::ofstream out(path, std::ios::trunc);
    if (!out) {
        if (error) *error = "cannot write '" + path + "'";
        return false;
    }
    out << "[Desktop Entry]\n"
        << "Type=Application\n"
        << "Name=SwiftDM\n"
        << "Comment=Download manager\n"
        << "Exec=" << exe << " --autostart\n"
        << "Icon=swiftdm\n"
        << "Terminal=false\n"
        << "X-GNOME-Autostart-enabled=true\n";
    out.flush();
    if (!out) {
        if (error) *error = "write failed for '" + path + "'";
        return false;
    }
    if (error) error->clear();
    return true;
}

}  // namespace swiftdm
