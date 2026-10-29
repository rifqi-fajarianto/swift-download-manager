#include <cstdio>
#include <cstdlib>
#include <sys/stat.h>
#include <fstream>
#include <iterator>
#include <string>

#include "core/util.h"
#include "store/settings.h"
#include "test_util.h"

using namespace swiftdm;

namespace {

void test_defaults() {
    Settings s = default_settings();
    CHECK_EQ(s.max_connections, 8);
    CHECK_EQ(s.connections_at_once, 0);
    CHECK_EQ(s.bandwidth_limit_kbps, 1024);
    CHECK(!s.bandwidth_limit_enabled);
    CHECK(s.category_folders);
    CHECK(s.completion_dialog);
    CHECK(!s.autostart);
    CHECK(s.show_tray_icon);
    CHECK(s.close_to_tray);
    CHECK(!s.open_folder_after_completion);
    CHECK(s.show_download_confirmation);
    CHECK_EQ(s.theme, std::string("dark"));
    CHECK_EQ(s.toolbar_size, std::string("normal"));
    CHECK(s.show_toolbar_text);
    CHECK_EQ(s.tray_icon_style, std::string("colorful"));
    CHECK_EQ(s.main_window_width, 1020);
    CHECK_EQ(s.main_window_height, 580);
    CHECK(!s.has_main_window_position);
    CHECK(!s.proxy.enabled());
    CHECK(s.default_agent.find("Mozilla/5.0") == 0);
    CHECK(s.download_dir.find("Downloads") != std::string::npos);
    CHECK(to_lower(s.temp_dir).find("swiftdm") != std::string::npos);
    CHECK(s.config_path.find("swiftdm.ini") != std::string::npos);
}

void test_roundtrip(const std::string& dir) {
    Settings s = default_settings();
    s.config_path = dir + "/swiftdm.ini";
    s.max_connections = 32;
    s.connections_at_once = 1;
    s.bandwidth_limit_kbps = 512;
    s.bandwidth_limit_enabled = true;
    s.use_ua_for_browser = true;
    s.default_agent = "TestAgent/1.0";
    s.download_dir = dir + "/downloads";
    s.category_folders = false;
    s.temp_dir = dir + "/tmp";
    s.server_creation_date = true;
    s.completion_dialog = false;
    s.autostart = true;
    s.show_tray_icon = false;
    s.close_to_tray = false;
    s.open_folder_after_completion = true;
    s.show_download_confirmation = false;
    s.theme = "light";
    s.toolbar_size = "small";
    s.show_toolbar_text = false;
    s.tray_icon_style = "simple";
    s.proxy.type = "socks5";
    s.proxy.address = "127.0.0.1";
    s.proxy.port = 1080;
    s.proxy.user = "user name";
    s.proxy.password = "p=a;ss #1";

    std::string err;
    CHECK(save_settings(s, &err));
    CHECK(err.empty());

    Settings loaded;
    CHECK(load_settings(s.config_path, loaded, &err));
    CHECK_EQ(loaded.max_connections, 32);
    CHECK_EQ(loaded.connections_at_once, 1);
    CHECK_EQ(loaded.bandwidth_limit_kbps, 512);
    CHECK(loaded.bandwidth_limit_enabled);
    CHECK(loaded.use_ua_for_browser);
    CHECK_EQ(loaded.default_agent, std::string("TestAgent/1.0"));
    CHECK_EQ(loaded.download_dir, s.download_dir);
    CHECK(!loaded.category_folders);
    CHECK_EQ(loaded.temp_dir, s.temp_dir);
    CHECK(loaded.server_creation_date);
    CHECK(!loaded.completion_dialog);
    CHECK(loaded.autostart);
    CHECK(!loaded.show_tray_icon);
    CHECK(!loaded.close_to_tray);
    CHECK(loaded.open_folder_after_completion);
    CHECK(!loaded.show_download_confirmation);
    CHECK_EQ(loaded.theme, std::string("light"));
    CHECK_EQ(loaded.toolbar_size, std::string("small"));
    CHECK(!loaded.show_toolbar_text);
    CHECK_EQ(loaded.tray_icon_style, std::string("simple"));
    CHECK_EQ(loaded.proxy.type, std::string("socks5"));
    CHECK_EQ(loaded.proxy.address, std::string("127.0.0.1"));
    CHECK_EQ(loaded.proxy.port, 1080);
    CHECK_EQ(loaded.proxy.user, std::string("user name"));
    CHECK_EQ(loaded.proxy.password, std::string("p=a;ss #1"));
    CHECK(loaded.proxy.enabled());
}

#ifndef _WIN32
mode_t file_mode(const std::string& path) {
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0) return static_cast<mode_t>(-1);
    return st.st_mode & 0777;
}
#endif

void test_private_atomic_config(const std::string& /*dir*/) {
    const std::string path = default_config_path();
    const std::string private_dir = path_to_utf8(path_from_utf8(path).parent_path());
    Settings s = default_settings();
    s.config_path = path;
    s.default_agent = "Agent/1.0\nInjectedKey = yes";
    s.proxy.password = "secret\nInjectedPassword = no";

    std::string error;
    CHECK(save_settings(s, &error));
    CHECK(error.empty());
#ifndef _WIN32
    CHECK_EQ(file_mode(private_dir), static_cast<mode_t>(0700));
    CHECK_EQ(file_mode(path), static_cast<mode_t>(0600));
#endif

    std::string content;
    {
        // Closed before the next save: Windows refuses to replace a file that
        // is still open, while POSIX would allow the rename.
        std::ifstream in(path);
        content.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    CHECK(content.find("DefaultAgent = Agent/1.0 InjectedKey = yes\n") != std::string::npos);
    CHECK(content.find("ProxyPassword = secret InjectedPassword = no\n") !=
          std::string::npos);
    CHECK(content.find("\nInjectedKey =") == std::string::npos);
    CHECK(content.find("\nInjectedPassword =") == std::string::npos);

    // Existing files are replaced without exposing a partially truncated
    // config, and each successful write restores strict mode on older files.
#ifndef _WIN32
    CHECK(::chmod(path.c_str(), 0644) == 0);
#endif
    s.default_agent = "SecondAgent/2.0";
    CHECK(save_settings(s, &error));
#ifndef _WIN32
    CHECK_EQ(file_mode(path), static_cast<mode_t>(0600));
    CHECK(::chmod(path.c_str(), 0644) == 0);
#endif
    Settings loaded;
    CHECK(load_settings(path, loaded, &error));
#ifndef _WIN32
    CHECK_EQ(file_mode(path), static_cast<mode_t>(0600));
#endif
    CHECK_EQ(loaded.default_agent, std::string("SecondAgent/2.0"));

    // Rename over a directory must fail without replacing or damaging it.
    const std::string obstructing_dir =
        path_to_utf8(path_from_utf8(private_dir) / "config-target-is-directory");
    CHECK(std::filesystem::create_directory(path_from_utf8(obstructing_dir)));
    const std::string marker = path_to_utf8(path_from_utf8(obstructing_dir) / "marker");
    { std::ofstream out(path_from_utf8(marker)); out << "preserve"; }
    s.config_path = obstructing_dir;
    CHECK(!save_settings(s, &error));
    std::ifstream marker_in(path_from_utf8(marker));
    std::string marker_text;
    marker_in >> marker_text;
    CHECK_EQ(marker_text, std::string("preserve"));
}

void test_missing_file(const std::string& dir) {
    Settings s;
    std::string err;
    CHECK(load_settings(dir + "/does-not-exist.ini", s, &err));
    CHECK_EQ(s.max_connections, 8);
    CHECK_EQ(s.config_path, dir + "/does-not-exist.ini");
}

void test_parsing_quirks(const std::string& dir) {
    const std::string path = dir + "/quirks.ini";
    std::ofstream out(path);
    out << "; comment line\n"
        << "# another comment\n"
        << "[general]\n"
        << "MaxConnections = 16\n"
        << "garbage line without equals\n"
        << "CategoryFolders = yes\n"
        << "CompletionDialog = 0\n"
        << "DownloadDirectory = ~/dl\n"
        << "DefaultAgent = Agent With Spaces/2.0\n"
        << "\n"
        << "[proxy]\n"
        << "SocksVersion = 5\n"
        << "ProxyAddress = proxy.example\n"
        << "ProxyPort = 3128\n";
    out.close();

    Settings s;
    std::string err;
    CHECK(load_settings(path, s, &err));
    CHECK_EQ(s.max_connections, 16);
    CHECK(s.category_folders);
    CHECK(!s.completion_dialog);
    CHECK_EQ(s.download_dir, home_dir() + "/dl");
    CHECK_EQ(s.default_agent, std::string("Agent With Spaces/2.0"));
    CHECK_EQ(s.proxy.type, std::string("socks5"));
    CHECK_EQ(s.proxy.address, std::string("proxy.example"));
    CHECK_EQ(s.proxy.port, 3128);

    // "http" token form.
    std::ofstream out2(dir + "/http.ini");
    out2 << "[proxy]\nSocksVersion = http\nProxyAddress = p.example\nProxyPort = 8080\n";
    out2.close();
    Settings s2;
    CHECK(load_settings(dir + "/http.ini", s2, &err));
    CHECK_EQ(s2.proxy.type, std::string("http"));
    CHECK(s2.proxy.enabled());
}

void test_autostart_entry(const std::string& dir) {
#ifdef _WIN32
    (void)dir;
    // The production setting changes the current-user Run key on Windows.
    // Avoid mutating the developer/CI user's actual login configuration here.
    CHECK(true);
#else
    std::string error;
    CHECK(apply_autostart(true, &error));
    CHECK(error.empty());

    const std::string path = dir + "/config/autostart/swiftdm.desktop";
    std::ifstream in(path);
    const std::string content((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
    CHECK(content.find("Exec=") != std::string::npos);
    // The executable path is a quoted Exec argument, so a path with spaces or
    // shell metacharacters survives the key-file parser and the shell.
    CHECK(content.find("Exec=\"") != std::string::npos);
    CHECK(content.find("\" --autostart\n") != std::string::npos);
    // The login entry must ask for the silent tray start; older entries lacked
    // this flag and showed the main window at login.
    CHECK(content.find(" --autostart\n") != std::string::npos);

    CHECK(apply_autostart(false, &error));
    CHECK(error.empty());
    std::ifstream removed(path);
    CHECK(!removed.good());
#endif
}

void test_bandwidth_toggle(const std::string& dir) {
    // Configs from before the toggle existed used a non-zero BandWidthLimit as
    // "limiter on", so the upgrade must keep them limited.
    {
        std::ofstream out(dir + "/legacy-limit.ini");
        out << "[general]\nBandWidthLimit = 256\n";
    }
    Settings legacy;
    std::string err;
    CHECK(load_settings(dir + "/legacy-limit.ini", legacy, &err));
    CHECK_EQ(legacy.bandwidth_limit_kbps, 256);
    CHECK(legacy.bandwidth_limit_enabled);

    // An explicit toggle off wins even when a non-zero value is stored.
    {
        std::ofstream out(dir + "/limit-off.ini");
        out << "[general]\nBandWidthLimit = 2048\nBandWidthLimitEnabled = 0\n";
    }
    Settings disabled;
    CHECK(load_settings(dir + "/limit-off.ini", disabled, &err));
    CHECK_EQ(disabled.bandwidth_limit_kbps, 2048);
    CHECK(!disabled.bandwidth_limit_enabled);
}

// Configs written before the [download] section existed keep the download
// options in [general]; they must still load.
void test_legacy_general_section(const std::string& dir) {
    const std::string path = dir + "/legacy.ini";
    {
        std::ofstream out(path);
        out << "[general]\n"
            << "MaxConnections = 16\n"
            << "BandWidthLimit = 256\n"
            << "BandWidthLimitEnabled = 1\n"
            << "UseUAgent = 1\n"
            << "DefaultAgent = LegacyAgent/1.0\n"
            << "DownloadDirectory = ~/legacy-dl\n"
            << "CategoryFolders = 0\n"
            << "TempDirectory = ~/legacy-tmp\n"
            << "ServerCreationDate = 1\n"
            << "CompletionDialog = 0\n"
            << "AppAutoStart = 1\n";
    }
    Settings s;
    std::string err;
    CHECK(load_settings(path, s, &err));
    CHECK_EQ(s.max_connections, 16);
    CHECK_EQ(s.bandwidth_limit_kbps, 256);
    CHECK(s.bandwidth_limit_enabled);
    CHECK(s.use_ua_for_browser);
    CHECK_EQ(s.default_agent, std::string("LegacyAgent/1.0"));
    CHECK_EQ(s.download_dir, home_dir() + "/legacy-dl");
    CHECK(!s.category_folders);
    CHECK_EQ(s.temp_dir, home_dir() + "/legacy-tmp");
    CHECK(s.server_creation_date);
    CHECK(!s.completion_dialog);
    CHECK(s.autostart);
    // The confirmation option did not exist in this config: the confirmation
    // stays on, so an old config keeps asking before a transfer starts.
    CHECK(s.show_download_confirmation);
}

// The dedicated [download] section wins over the legacy location, whatever
// the order of the sections in the file is.
void test_download_section_wins(const std::string& dir) {
    const std::string path = dir + "/sections.ini";
    {
        std::ofstream out(path);
        out << "[download]\n"
            << "MaxConnections = 32\n"
            << "DefaultAgent = NewAgent/3.0\n"
            << "\n[general]\n"
            << "MaxConnections = 16\n"
            << "DefaultAgent = OldAgent/1.0\n"
            << "CompletionDialog = 0\n";
    }
    Settings s;
    std::string err;
    CHECK(load_settings(path, s, &err));
    CHECK_EQ(s.max_connections, 32);
    CHECK_EQ(s.default_agent, std::string("NewAgent/3.0"));
    // The general options are still read from [general].
    CHECK(!s.completion_dialog);
}

// Unknown interface values keep the defaults; known ones are accepted
// regardless of case.
void test_interface_values(const std::string& dir) {
    const std::string path = dir + "/interface.ini";
    {
        std::ofstream out(path);
        out << "[interface]\n"
            << "Theme = neon\n"
            << "ToolbarSize = huge\n"
            << "TrayIconStyle = neon\n"
            << "ShowToolbarText = 0\n";
    }
    Settings s;
    std::string err;
    CHECK(load_settings(path, s, &err));
    CHECK_EQ(s.theme, std::string("dark"));
    CHECK_EQ(s.toolbar_size, std::string("normal"));
    CHECK_EQ(s.tray_icon_style, std::string("colorful"));
    CHECK(!s.show_toolbar_text);

    {
        std::ofstream out(dir + "/interface-valid.ini");
        out << "[interface]\n"
            << "Theme = System\n"
            << "ToolbarSize = Small\n"
            << "TrayIconStyle = Simple\n"
            << "ShowToolbarText = yes\n";
    }
    Settings valid;
    CHECK(load_settings(dir + "/interface-valid.ini", valid, &err));
    CHECK_EQ(valid.theme, std::string("system"));
    CHECK_EQ(valid.toolbar_size, std::string("small"));
    CHECK_EQ(valid.tray_icon_style, std::string("simple"));
    CHECK(valid.show_toolbar_text);

    CHECK(is_valid_theme("dark"));
    CHECK(is_valid_theme("light"));
    CHECK(is_valid_theme("system"));
    CHECK(!is_valid_theme("neon"));
    CHECK(is_valid_toolbar_size("normal"));
    CHECK(is_valid_toolbar_size("small"));
    CHECK(!is_valid_toolbar_size("huge"));
    CHECK(is_valid_tray_icon_style("colorful"));
    CHECK(is_valid_tray_icon_style("simple"));
    CHECK(!is_valid_tray_icon_style("neon"));
}

// The written file uses one section per tab.
void test_written_sections(const std::string& dir) {
    Settings s = default_settings();
    s.config_path = dir + "/written.ini";
    std::string err;
    CHECK(save_settings(s, &err));

    std::ifstream in(s.config_path);
    const std::string content((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
    for (const char* section : {"[general]", "[download]", "[interface]", "[proxy]"}) {
        CHECK(content.find(section) != std::string::npos);
    }
    // The download options live in their own section now.
    const size_t download = content.find("[download]");
    const size_t interface = content.find("[interface]");
    CHECK(download != std::string::npos && interface != std::string::npos);
    if (download != std::string::npos && interface != std::string::npos) {
        CHECK(download < interface);
        CHECK(content.find("MaxConnections", download) < interface);
        CHECK(content.find("Theme", interface) != std::string::npos);
        CHECK(content.find("WindowWidth", interface) != std::string::npos);
        CHECK(content.find("WindowHeight", interface) != std::string::npos);
    }
    // The confirmation option belongs to the general section.
    const size_t general = content.find("[general]");
    CHECK(general != std::string::npos);
    if (general != std::string::npos && download != std::string::npos) {
        CHECK(content.find("ShowDownloadConfirmationDialog", general) < download);
    }
}

// The confirmation option can be switched off; a config without the key keeps
// the confirmation on.
void test_confirmation_option(const std::string& dir) {
    {
        std::ofstream out(dir + "/confirmation-off.ini");
        out << "[general]\nShowDownloadConfirmationDialog = 0\n";
    }
    Settings off;
    std::string err;
    CHECK(load_settings(dir + "/confirmation-off.ini", off, &err));
    CHECK(!off.show_download_confirmation);

    {
        std::ofstream out(dir + "/confirmation-on.ini");
        out << "[general]\nShowDownloadConfirmationDialog = 1\n";
    }
    Settings on;
    CHECK(load_settings(dir + "/confirmation-on.ini", on, &err));
    CHECK(on.show_download_confirmation);

    {
        std::ofstream out(dir + "/confirmation-missing.ini");
        out << "[general]\nCompletionDialog = 1\n";
    }
    Settings missing;
    CHECK(load_settings(dir + "/confirmation-missing.ini", missing, &err));
    CHECK(missing.show_download_confirmation);
}

// The main window size is optional state: valid values round-trip, malformed
// ones keep the defaults, and a position needs both coordinates.
void test_window_geometry(const std::string& dir) {
    const std::string path = dir + "/geometry.ini";
    {
        std::ofstream out(path);
        out << "[interface]\n"
            << "WindowWidth = 1280\n"
            << "WindowHeight = 800\n"
            << "WindowX = -120\n"
            << "WindowY = 40\n";
    }
    Settings saved;
    std::string err;
    CHECK(load_settings(path, saved, &err));
    CHECK_EQ(saved.main_window_width, 1280);
    CHECK_EQ(saved.main_window_height, 800);
    CHECK(saved.has_main_window_position);
    CHECK_EQ(saved.main_window_x, -120);
    CHECK_EQ(saved.main_window_y, 40);

    {
        std::ofstream out(dir + "/geometry-invalid.ini");
        out << "[interface]\n"
            << "WindowWidth = 10\n"
            << "WindowHeight = nope\n"
            << "WindowX = 100\n";  // Y missing: the position stays unset.
    }
    Settings invalid = default_settings();
    CHECK(load_settings(dir + "/geometry-invalid.ini", invalid, &err));
    CHECK_EQ(invalid.main_window_width, 1020);
    CHECK_EQ(invalid.main_window_height, 580);
    CHECK(!invalid.has_main_window_position);
    CHECK_EQ(invalid.main_window_x, 0);

    // A saved position is written back, and without one the file keeps no
    // WindowX/WindowY keys so the window manager still places the window.
    Settings roundtrip = default_settings();
    roundtrip.config_path = dir + "/geometry-written.ini";
    roundtrip.main_window_width = 900;
    roundtrip.main_window_height = 640;
    roundtrip.main_window_x = 15;
    roundtrip.main_window_y = 25;
    roundtrip.has_main_window_position = true;
    CHECK(save_settings(roundtrip, &err));
    Settings reloaded;
    CHECK(load_settings(roundtrip.config_path, reloaded, &err));
    CHECK_EQ(reloaded.main_window_width, 900);
    CHECK_EQ(reloaded.main_window_height, 640);
    CHECK(reloaded.has_main_window_position);
    CHECK_EQ(reloaded.main_window_x, 15);
    CHECK_EQ(reloaded.main_window_y, 25);

    Settings no_position = default_settings();
    no_position.config_path = dir + "/geometry-no-position.ini";
    no_position.main_window_width = 800;
    no_position.main_window_height = 600;
    CHECK(save_settings(no_position, &err));
    Settings no_position_loaded;
    CHECK(load_settings(no_position.config_path, no_position_loaded, &err));
    CHECK_EQ(no_position_loaded.main_window_width, 800);
    CHECK_EQ(no_position_loaded.main_window_height, 600);
    CHECK(!no_position_loaded.has_main_window_position);
}

}  // namespace

int main() {
    const std::string dir = testutil::make_temp_dir("settings");
    // Isolate HOME/XDG so defaults and tilde expansion are deterministic.
#ifndef _WIN32
    setenv("HOME", (dir + "/home").c_str(), 1);
    setenv("XDG_CONFIG_HOME", (dir + "/config").c_str(), 1);
    setenv("XDG_DATA_HOME", (dir + "/data").c_str(), 1);
#else
    // Keep every write inside the test root: an empty APPDATA would resolve to
    // a relative "/AppData/Roaming" path at the drive root.
    const std::wstring app_data = path_from_utf8(dir + "/appdata").wstring();
    const std::wstring local_app_data = path_from_utf8(dir + "/localappdata").wstring();
    const std::wstring profile = path_from_utf8(dir + "/home").wstring();
    _wputenv_s(L"APPDATA", app_data.c_str());
    _wputenv_s(L"LOCALAPPDATA", local_app_data.c_str());
    _wputenv_s(L"USERPROFILE", profile.c_str());
#endif

    test_defaults();
    test_roundtrip(dir);
    test_private_atomic_config(dir);
    test_missing_file(dir);
    test_parsing_quirks(dir);
    test_autostart_entry(dir);
    test_bandwidth_toggle(dir);
    test_legacy_general_section(dir);
    test_download_section_wins(dir);
    test_interface_values(dir);
    test_written_sections(dir);
    test_confirmation_option(dir);
    test_window_geometry(dir);

    testutil::cleanup_dir(dir);
    return testutil::finish("test_settings");
}
