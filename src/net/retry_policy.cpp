#include "net/retry_policy.h"

#include <algorithm>
#include <cctype>
#include <iterator>

#include "core/util.h"
#include "net/http.h"

namespace swiftdm {
namespace {

// Wording that hosts use for "you may not open another connection". Kept short
// and specific: a generic 503 page must stay a normal server error.
const char* const kLimitMarkers[] = {
    "max_concurrent_downloads",
    "concurrent",
    "parallel",
    "too many connections",
    "open download connections",
    "maximum number of open",
    "download in progress",
    "download already in progress",
    "download limit",
    "simultaneous",
    "connection limit",
};

std::string lower_copy(const std::string& text) {
    std::string out(text);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

}  // namespace

bool body_mentions_connection_limit(const std::string& body, const std::string& content_type) {
    (void)content_type;  // the wording is what matters, not the media type
    if (body.empty()) return false;
    const std::string lower = lower_copy(body);
    return std::any_of(std::begin(kLimitMarkers), std::end(kLimitMarkers),
                       [&](const char* marker) { return lower.find(marker) != std::string::npos; });
}

bool is_concurrency_refusal(long status, const std::string& body,
                            const std::string& content_type) {
    if (status != 403 && status != 503) return false;
    return body_mentions_connection_limit(body, content_type);
}

bool error_is_concurrency_refusal(const std::string& error) {
    const int status = http_error_status(error);
    if (status == 0) return false;
    return is_concurrency_refusal(status, error, {});
}

int64_t parse_retry_after_seconds(const std::string& value) {
    const std::string text = trim(value);
    if (text.empty()) return 0;
    int64_t seconds = 0;
    for (char c : text) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return 0;  // HTTP-date form
        if (seconds > 86400) return 86400;                           // clamp absurd waits
        seconds = seconds * 10 + (c - '0');
    }
    return std::min<int64_t>(seconds, 86400);
}

AdaptiveConcurrency::AdaptiveConcurrency(int limit) : limit_(std::max(1, limit)) {}

void AdaptiveConcurrency::on_refusal() {
    // Give up exactly one slot. A batch refused at once (eight ranges against a
    // host that allows two) therefore lands on the host's real value in that
    // same batch: 8 -> 7 -> 6 -> ... -> 2.
    limit_ = std::max(1, limit_ - 1);
}

int64_t AdaptiveConcurrency::defer_backoff_ms(int deferrals) {
    if (deferrals <= 0) return 0;
    const int64_t shift = std::min<int64_t>(deferrals - 1, 2);
    return std::min<int64_t>(8000, 2000LL << shift);
}

}  // namespace swiftdm
