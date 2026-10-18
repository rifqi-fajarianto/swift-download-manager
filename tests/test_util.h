#pragma once

// Shared helpers for CLI/backend tests (no GUI automation, per AGENTS.md).

#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <unistd.h>

namespace testutil {

inline int g_failures = 0;

inline std::string make_temp_dir(const std::string& tag) {
    std::error_code ec;
    std::filesystem::path base = "/tmp/opencode";
    if (!std::filesystem::is_directory(base, ec)) {
        base = std::filesystem::temp_directory_path(ec);
    }
    const std::filesystem::path dir =
        base / ("swiftdm-test-" + tag + "-" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir.string();
}

inline void cleanup_dir(const std::string& dir) {
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

inline void report_failure(const char* file, int line, const char* expr) {
    std::fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expr);
    ++g_failures;
}

inline int finish(const char* name) {
    if (g_failures == 0) {
        std::printf("%s: all tests passed\n", name);
        return 0;
    }
    std::fprintf(stderr, "%s: %d failure(s)\n", name, g_failures);
    return 1;
}

}  // namespace testutil

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) testutil::report_failure(__FILE__, __LINE__, #cond); \
    } while (0)

#define CHECK_EQ(a, b)                                                     \
    do {                                                                   \
        const auto _va = (a);                                              \
        const auto _vb = (b);                                              \
        if (!(_va == _vb)) {                                               \
            testutil::report_failure(__FILE__, __LINE__, #a " == " #b);    \
        }                                                                  \
    } while (0)
