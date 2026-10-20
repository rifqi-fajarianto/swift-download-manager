#include <cstdio>
#include <cstdlib>
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
    CHECK(!s.proxy.enabled());
    CHECK(s.default_agent.find("Mozilla/5.0") == 0);
    CHECK(s.download_dir.find("/Downloads") != std::string::npos);
    CHECK(s.temp_dir.find("swiftdm") != std::string::npos);
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
    CHECK_EQ(loaded.proxy.type, std::string("socks5"));
    CHECK_EQ(loaded.proxy.address, std::string("127.0.0.1"));
    CHECK_EQ(loaded.proxy.port, 1080);
    CHECK_EQ(loaded.proxy.user, std::string("user name"));
    CHECK_EQ(loaded.proxy.password, std::string("p=a;ss #1"));
    CHECK(loaded.proxy.enabled());
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
    std::string error;
    CHECK(apply_autostart(true, &error));
    CHECK(error.empty());

    const std::string path = dir + "/config/autostart/swiftdm.desktop";
    std::ifstream in(path);
    const std::string content((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
    CHECK(content.find("Exec=") != std::string::npos);
    // The login entry must ask for the silent tray start; older entries lacked
    // this flag and showed the main window at login.
    CHECK(content.find(" --autostart\n") != std::string::npos);

    CHECK(apply_autostart(false, &error));
    CHECK(error.empty());
    std::ifstream removed(path);
    CHECK(!removed.good());
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

}  // namespace

int main() {
    const std::string dir = testutil::make_temp_dir("settings");
    // Isolate HOME/XDG so defaults and tilde expansion are deterministic.
    setenv("HOME", (dir + "/home").c_str(), 1);
    setenv("XDG_CONFIG_HOME", (dir + "/config").c_str(), 1);
    setenv("XDG_DATA_HOME", (dir + "/data").c_str(), 1);

    test_defaults();
    test_roundtrip(dir);
    test_missing_file(dir);
    test_parsing_quirks(dir);
    test_autostart_entry(dir);
    test_bandwidth_toggle(dir);

    testutil::cleanup_dir(dir);
    return testutil::finish("test_settings");
}
