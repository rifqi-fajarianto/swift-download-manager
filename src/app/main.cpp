#include <cstdio>
#include <cstring>
#include <string>

#include "core/log.h"
#include "core/util.h"
#include "engine/download_engine.h"
#include "store/settings.h"
#include "swiftdm/version.h"

#ifdef SWIFTSDM_WITH_GUI
#include <QApplication>
#include <QString>

#include "ui/icons.h"
#include "ui/main_window.h"
#include "ui/theme.h"
#endif

namespace {

void print_usage(std::FILE* out) {
    std::fprintf(out,
        "SwiftDM %s — segmented, resumable download manager\n"
        "\n"
        "Usage:\n"
        "  swiftdm                       Start the GUI\n"
        "  swiftdm --gui                 Start the GUI\n"
        "  swiftdm get <url> [options]   Download a URL (headless)\n"
        "  swiftdm --version             Print version and exit\n"
        "  swiftdm --help                Print this help and exit\n"
        "\n"
        "Options for 'get':\n"
        "  --dir DIR           Output directory (default: settings)\n"
        "  --name NAME         Output filename\n"
        "  --connections N     Connections per download (default: settings)\n"
        "  --limit KBPS        Per-download bandwidth limit (0 = unlimited)\n"
        "  --no-resume         Ignore existing partial state\n"
        "  --referer URL       Referer header\n"
        "  --header \"K: V\"     Extra request header (repeatable)\n"
        "  --ua STRING         User-Agent override\n"
        "  --post DATA         POST body (disables segmented transfer)\n"
        "  --config PATH       Config file (default: ~/.config/swiftdm/swiftdm.ini)\n"
        "  --pause-after BYTES Pause once at least BYTES were downloaded (testing)\n"
        "  --quiet             Do not print progress lines\n"
        "  --log-level LEVEL   trace|debug|info|warn|error (default: warn)\n"
        "\n"
        "Exit codes: 0 completed, 1 failed, 2 usage error, 3 paused.\n",
        swiftdm::kVersionString);
}

#ifdef SWIFTSDM_WITH_GUI
int run_gui(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("SwiftDM"));
    QApplication::setApplicationVersion(QString::fromLatin1(swiftdm::kVersionString));
    QApplication::setOrganizationName(QStringLiteral("SwiftDM"));
    QApplication::setWindowIcon(swiftdm::ui::application_icon());
    swiftdm::ui::apply_dark_theme(app);

    swiftdm::Settings settings;
    std::string error;
    swiftdm::load_settings(swiftdm::default_config_path(), settings, &error);
    if (!error.empty()) {
        std::fprintf(stderr, "warning: %s\n", error.c_str());
    }

    swiftdm::ui::MainWindow window(std::move(settings));
    window.show();
    return app.exec();
}
#endif

bool parse_log_level(const char* value, swiftdm::LogLevel& out) {
    if (std::strcmp(value, "trace") == 0) { out = swiftdm::LogLevel::Trace; return true; }
    if (std::strcmp(value, "debug") == 0) { out = swiftdm::LogLevel::Debug; return true; }
    if (std::strcmp(value, "info") == 0) { out = swiftdm::LogLevel::Info; return true; }
    if (std::strcmp(value, "warn") == 0) { out = swiftdm::LogLevel::Warn; return true; }
    if (std::strcmp(value, "error") == 0) { out = swiftdm::LogLevel::Error; return true; }
    return false;
}

bool parse_u64(const char* text, uint64_t& out) {
    char* end = nullptr;
    const unsigned long long v = std::strtoull(text, &end, 10);
    if (end == text || (end && *end != '\0')) return false;
    out = v;
    return true;
}

void print_progress(const swiftdm::ProgressInfo& info, bool quiet) {
    if (quiet) return;
    std::string line;
    if (info.total > 0) {
        line = "  " + std::to_string(info.percent) + "%  " +
               swiftdm::format_bytes(info.done, 3) + " / " +
               swiftdm::format_bytes(info.total, 3);
    } else {
        line = "  " + swiftdm::format_bytes(info.done, 3);
    }
    if (info.speed_bps > 0) {
        line += "  " + swiftdm::format_speed(info.speed_bps, 3);
        if (info.total > info.done) {
            const auto eta = static_cast<int64_t>(
                static_cast<double>(info.total - info.done) / info.speed_bps);
            line += "  ETA " + swiftdm::format_duration(eta);
        }
    }
    std::fprintf(stdout, "\r%-90s", line.c_str());
    std::fflush(stdout);
}

int run_get(int argc, char** argv) {
    if (argc < 1) {
        std::fprintf(stderr, "error: 'get' requires a URL\n");
        return 2;
    }
    const std::string url = argv[0];

    swiftdm::DownloadRequest request;
    request.url = url;
    std::string config_path = swiftdm::default_config_path();
    uint64_t pause_after = 0;
    bool quiet = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto need_value = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: %s requires a value\n", name);
                return nullptr;
            }
            return argv[++i];
        };
        if (arg == "--dir") {
            const char* v = need_value("--dir"); if (!v) return 2;
            request.output_dir = swiftdm::expand_tilde(v);
        } else if (arg == "--name") {
            const char* v = need_value("--name"); if (!v) return 2;
            request.filename = v;
        } else if (arg == "--connections") {
            const char* v = need_value("--connections"); if (!v) return 2;
            request.connections = std::atoi(v);
        } else if (arg == "--limit") {
            const char* v = need_value("--limit"); if (!v) return 2;
            request.bandwidth_limit_kbps = std::atoi(v);
        } else if (arg == "--no-resume") {
            request.resume = false;
        } else if (arg == "--referer") {
            const char* v = need_value("--referer"); if (!v) return 2;
            request.referer = v;
        } else if (arg == "--header") {
            const char* v = need_value("--header"); if (!v) return 2;
            const std::string h = v;
            const auto colon = h.find(':');
            if (colon == std::string::npos) {
                std::fprintf(stderr, "error: --header expects 'Name: Value'\n");
                return 2;
            }
            request.headers.push_back({swiftdm::trim(h.substr(0, colon)),
                                       swiftdm::trim(h.substr(colon + 1))});
        } else if (arg == "--ua") {
            const char* v = need_value("--ua"); if (!v) return 2;
            request.user_agent = v;
        } else if (arg == "--post") {
            const char* v = need_value("--post"); if (!v) return 2;
            request.post_data = v;
        } else if (arg == "--config") {
            const char* v = need_value("--config"); if (!v) return 2;
            config_path = v;
        } else if (arg == "--pause-after") {
            const char* v = need_value("--pause-after"); if (!v) return 2;
            if (!parse_u64(v, pause_after)) {
                std::fprintf(stderr, "error: --pause-after expects a byte count\n");
                return 2;
            }
        } else if (arg == "--quiet") {
            quiet = true;
        } else if (arg == "--log-level") {
            const char* v = need_value("--log-level"); if (!v) return 2;
            swiftdm::LogLevel level{};
            if (!parse_log_level(v, level)) {
                std::fprintf(stderr, "error: invalid log level '%s'\n", v);
                return 2;
            }
            swiftdm::log_set_level(level);
        } else {
            std::fprintf(stderr, "error: unknown option '%s'\n", arg.c_str());
            return 2;
        }
    }

    swiftdm::Settings settings;
    std::string settings_error;
    swiftdm::load_settings(config_path, settings, &settings_error);

    swiftdm::DownloadEngine engine(settings);
    uint64_t last_done = 0;
    const auto result = engine.run(request, [&](const swiftdm::ProgressInfo& info) {
        last_done = info.done;
        print_progress(info, quiet);
        if (pause_after > 0 && info.done >= pause_after) return false;
        return true;
    });
    if (!quiet) std::fprintf(stdout, "\n");

    switch (result.status) {
        case swiftdm::DownloadResult::Status::Completed:
            std::printf("completed: %s (%s)\n", result.path.c_str(),
                        swiftdm::format_bytes(result.size, 3).c_str());
            return 0;
        case swiftdm::DownloadResult::Status::Paused:
            std::printf("paused: %s (%s downloaded)\n",
                        result.path.empty() ? "partial data" : result.path.c_str(),
                        swiftdm::format_bytes(last_done, 3).c_str());
            return 3;
        case swiftdm::DownloadResult::Status::Failed:
            std::fprintf(stderr, "failed: %s\n", result.error.c_str());
            return 1;
    }
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    swiftdm::log_set_level(swiftdm::LogLevel::Warn);

    if (argc <= 1) {
#ifdef SWIFTSDM_WITH_GUI
        return run_gui(argc, argv);
#else
        print_usage(stderr);
        return 2;
#endif
    }

    const std::string command = argv[1];
    if (command == "--version") {
        std::printf("SwiftDM %s\n", swiftdm::kVersionString);
        return 0;
    }
    if (command == "--help" || command == "-h" || command == "help") {
        print_usage(stdout);
        return 0;
    }
#ifdef SWIFTSDM_WITH_GUI
    if (command == "--gui") {
        return run_gui(argc, argv);
    }
#endif
    if (command == "get") {
        return run_get(argc - 2, argv + 2);
    }

    std::fprintf(stderr, "error: unknown command '%s'\n", command.c_str());
    print_usage(stderr);
    return 2;
}
