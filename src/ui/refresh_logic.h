#pragma once

#include <cstdint>
#include <string>

namespace swiftdm::ui {

// What a browser capture means while a download waits for a refreshed address.
enum class RefreshCaptureDecision {
    // No download is waiting: the capture is an ordinary new download.
    NotWaiting,
    // A download waits and the capture names the same file: ask to confirm.
    Prompt,
    // A download waits but the capture is a different file: still ask (with a
    // mismatch warning), never create a new download.
    PromptNameMismatch,
};

// Decides how a browser capture is handled. While a refresh is pending every
// capture is routed to the confirmation prompt, so no new download row can be
// created before the wait ends.
RefreshCaptureDecision decide_refresh_capture(bool refresh_pending, bool name_match);

// True when the manual "New URL" action must be blocked with a warning. A
// waiting refresh owns the next capture, so a manual download would steal it.
bool refresh_blocks_new_download(bool refresh_pending);

// How long a capture that repeats the address just applied is ignored. The
// browser extension retries a command it considers unanswered, and the app also
// receives duplicate captures while a transfer is being restarted; without this
// window each repeat would open a fresh download confirmation.
inline constexpr int64_t kRefreshEchoWindowMs = 15000;

// True when `incoming_url` is the address that was just applied by a refresh,
// still inside the echo window. Such a capture is a repeat of the refresh
// handoff, not a new download, so it must be dropped silently.
bool should_ignore_refresh_echo(const std::string& incoming_url,
                                const std::string& applied_url, int64_t elapsed_ms);

}  // namespace swiftdm::ui
