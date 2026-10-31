#include "ui/refresh_logic.h"

namespace swiftdm::ui {

RefreshCaptureDecision decide_refresh_capture(bool refresh_pending, bool name_match) {
    if (!refresh_pending) return RefreshCaptureDecision::NotWaiting;
    return name_match ? RefreshCaptureDecision::Prompt
                      : RefreshCaptureDecision::PromptNameMismatch;
}

bool refresh_blocks_new_download(bool refresh_pending) { return refresh_pending; }

bool should_ignore_refresh_echo(const std::string& incoming_url,
                                const std::string& applied_url, int64_t elapsed_ms) {
    if (applied_url.empty() || incoming_url != applied_url) return false;
    return elapsed_ms >= 0 && elapsed_ms <= kRefreshEchoWindowMs;
}

}  // namespace swiftdm::ui
