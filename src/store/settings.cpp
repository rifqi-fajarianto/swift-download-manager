#include "store/settings.h"

#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
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

void apply_key(Settings& s, const std::string& section, const std::string& key,
               const std::string& value, bool* saw_bandwidth_toggle = nullptr) {
    const std::string k = to_lower(key);
    if (section == "general") {
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
        else if (k == "completiondialog") s.completion_dialog = parse_bool(value, s.completion_dialog);
        else if (k == "appautostart") s.autostart = parse_bool(value, s.autostart);
    } else if (section == "proxy") {
        if (k == "socksversion") s.proxy.type = proxy_token_to_type(value);
        else if (k == "proxyaddress") s.proxy.address = trim(value);
        else if (k == "proxyport") parse_int(value, s.proxy.port, s.proxy.port);
        else if (k == "proxyuser") s.proxy.user = value;
        else if (k == "proxypassword") s.proxy.password = value;
    }
}

}  // namespace

std::string default_config_path() { return xdg_config_home() + "/swiftdm/swiftdm.ini"; }

std::string default_temp_dir() { return xdg_data_home() + "/swiftdm/tmp"; }

std::string default_database_path() { return xdg_data_home() + "/swiftdm/swiftdm.db"; }

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

    std::string section = "general";
    bool saw_bandwidth_toggle = false;
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
        apply_key(out, section, key, value, &saw_bandwidth_toggle);
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
        << "CompletionDialog = " << (settings.completion_dialog ? 1 : 0) << "\n"
        << "AppAutoStart = " << (settings.autostart ? 1 : 0) << "\n"
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
        << "Exec=" << exe << "\n"
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
