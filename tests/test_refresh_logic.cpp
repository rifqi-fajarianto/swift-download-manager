// Headless check of the refresh-download-address routing rules. The two
// guarantees are: while a refresh waits, every browser capture goes to the
// confirmation prompt (never to a new download), and the manual New URL action
// is blocked. No GUI is created (AGENTS.md §6).

#include <cstdio>

#include "ui/refresh_logic.h"

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

using swiftdm::ui::decide_refresh_capture;
using swiftdm::ui::RefreshCaptureDecision;
using swiftdm::ui::refresh_blocks_new_download;

}  // namespace

int main() {
    // No refresh pending: a capture is an ordinary new download.
    check(decide_refresh_capture(false, true) == RefreshCaptureDecision::NotWaiting,
          "no pending refresh, matching name -> new download");
    check(decide_refresh_capture(false, false) == RefreshCaptureDecision::NotWaiting,
          "no pending refresh, different name -> new download");

    // A refresh is pending: both matching and non-matching captures prompt.
    check(decide_refresh_capture(true, true) == RefreshCaptureDecision::Prompt,
          "pending refresh, matching name -> confirm");
    check(decide_refresh_capture(true, false) == RefreshCaptureDecision::PromptNameMismatch,
          "pending refresh, different name -> confirm with mismatch warning");

    // The critical regression: a pending refresh must never yield NotWaiting,
    // otherwise a second download row would be created during the wait.
    for (const bool match : {true, false}) {
        check(decide_refresh_capture(true, match) != RefreshCaptureDecision::NotWaiting,
              "pending refresh never creates a new download");
    }

    // Manual New URL is blocked only while a refresh waits.
    check(!refresh_blocks_new_download(false), "New URL allowed when idle");
    check(refresh_blocks_new_download(true), "New URL blocked while refreshing");

    // Echo guard: the address just applied is dropped inside the window. This is
    // what stops the extension's command retry from opening a second (and third,
    // and fourth) download confirmation for the refreshed file.
    using swiftdm::ui::kRefreshEchoWindowMs;
    using swiftdm::ui::should_ignore_refresh_echo;
    const std::string applied = "https://cdn.example.com/file.zip?sig=new";
    const std::string other = "https://cdn.example.com/another.zip";

    check(should_ignore_refresh_echo(applied, applied, 0),
          "echo at once is ignored");
    check(should_ignore_refresh_echo(applied, applied, 250),
          "echo after the extension retry delay is ignored");
    check(should_ignore_refresh_echo(applied, applied, kRefreshEchoWindowMs),
          "echo at the window edge is ignored");
    check(!should_ignore_refresh_echo(applied, applied, kRefreshEchoWindowMs + 1),
          "a capture after the window is a normal download again");
    check(!should_ignore_refresh_echo(other, applied, 250),
          "a different address is never treated as an echo");
    check(!should_ignore_refresh_echo(applied, "", 250),
          "no applied address means no echo");
    check(!should_ignore_refresh_echo("", "", 250),
          "empty addresses never match");

    if (failures == 0) {
        std::printf("refresh_logic: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "refresh_logic: %d check(s) failed\n", failures);
    return 1;
}
