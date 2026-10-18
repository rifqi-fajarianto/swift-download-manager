#include <cstdio>
#include <string>

#include "core/log.h"
#include "swiftdm/version.h"

namespace {
int g_failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,   \
                         #cond);                                           \
            ++g_failures;                                                  \
        }                                                                  \
    } while (0)
}  // namespace

int main() {
    CHECK(swiftdm::kVersionMajor == SWIFTSDM_VERSION_MAJOR);
    CHECK(swiftdm::kVersionMinor == SWIFTSDM_VERSION_MINOR);
    CHECK(swiftdm::kVersionPatch == SWIFTSDM_VERSION_PATCH);
    CHECK(std::string(swiftdm::kVersionString).find('.') != std::string::npos);

    CHECK(swiftdm::format_seconds(-1) == "--");
    CHECK(swiftdm::format_seconds(0) == "0s");
    CHECK(swiftdm::format_seconds(59) == "59s");
    CHECK(swiftdm::format_seconds(60) == "1m 00s");
    CHECK(swiftdm::format_seconds(3661) == "1h 01m 01s");

    if (g_failures == 0) {
        std::printf("all tests passed (version %s)\n", swiftdm::kVersionString);
        return 0;
    }
    std::fprintf(stderr, "%d test(s) failed\n", g_failures);
    return 1;
}
