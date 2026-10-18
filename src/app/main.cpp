#include <cstdio>
#include <cstring>
#include <string>

#include "core/log.h"
#include "swiftdm/version.h"

namespace {

void print_usage(std::FILE* out) {
    std::fprintf(out,
        "SwiftDM %s — segmented, resumable download manager\n"
        "\n"
        "Usage: swiftdm [options]\n"
        "\n"
        "Options:\n"
        "  --version        Print version and exit\n"
        "  --help           Print this help and exit\n"
        "  --log-level <l>  trace|debug|info|warn|error (default: info)\n"
        "\n"
        "The GUI is not built yet in this milestone; CLI engine modes will be\n"
        "added as the engine lands (see docs/spec/).\n",
        swiftdm::kVersionString);
}

bool parse_log_level(const char* value, swiftdm::LogLevel& out) {
    if (std::strcmp(value, "trace") == 0) { out = swiftdm::LogLevel::Trace; return true; }
    if (std::strcmp(value, "debug") == 0) { out = swiftdm::LogLevel::Debug; return true; }
    if (std::strcmp(value, "info")  == 0) { out = swiftdm::LogLevel::Info;  return true; }
    if (std::strcmp(value, "warn")  == 0) { out = swiftdm::LogLevel::Warn;  return true; }
    if (std::strcmp(value, "error") == 0) { out = swiftdm::LogLevel::Error; return true; }
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    bool want_version = false;
    bool want_help = false;

    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (std::strcmp(arg, "--version") == 0) {
            want_version = true;
        } else if (std::strcmp(arg, "--help") == 0 || std::strcmp(arg, "-h") == 0) {
            want_help = true;
        } else if (std::strcmp(arg, "--log-level") == 0) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: --log-level requires a value\n");
                return 2;
            }
            swiftdm::LogLevel level{};
            if (!parse_log_level(argv[++i], level)) {
                std::fprintf(stderr, "error: invalid log level '%s'\n", argv[i]);
                return 2;
            }
            swiftdm::log_set_level(level);
        } else {
            std::fprintf(stderr, "error: unknown option '%s'\n", arg);
            print_usage(stderr);
            return 2;
        }
    }

    if (want_version) {
        std::printf("SwiftDM %s\n", swiftdm::kVersionString);
        return 0;
    }
    if (want_help || argc <= 1) {
        print_usage(want_help ? stdout : stderr);
        return want_help ? 0 : 1;
    }

    swiftdm::log_info(std::string("SwiftDM ") + swiftdm::kVersionString + " — no command given");
    return 1;
}
