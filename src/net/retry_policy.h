#pragma once

#include <cstdint>
#include <string>

namespace swiftdm {

// Hosts with a per-account connection limit answer "download already in
// progress" / "maximum number of open download connections" when a second or
// third range arrives. That is not a server outage: retrying the same way only
// burns the retry budget while every extra connection is refused again. These
// refusals lower the number of parallel ranges instead (see
// docs/spec/03-engine.md §5).
bool body_mentions_connection_limit(const std::string& body, const std::string& content_type);

// True for a response that asks for fewer parallel connections: a 403/503 whose
// body names the connection limit. A bare 429 (rate limit) is not a concurrency
// refusal - it keeps the normal retry path, which honours Retry-After.
bool is_concurrency_refusal(long status, const std::string& body,
                            const std::string& content_type);

// Same test on a finished error message ("HTTP/1.1 503 Service Unavailable -
// Download In Progress — UploadHaven"), where the host's own wording has
// already been condensed into the text.
bool error_is_concurrency_refusal(const std::string& error);

// "Retry-After: 120" -> 120. 0 when the header is missing or uses the HTTP-date
// form; the caller then falls back to its own backoff.
int64_t parse_retry_after_seconds(const std::string& value);

// Parallel-range budget that narrows itself whenever the host refuses one more
// connection. It never grows back during a run: the goal is to finish, not to
// probe the host's limit again.
class AdaptiveConcurrency {
  public:
    // A deferral is not a retry - the refused request never had a fair chance -
    // so it does not consume the retry budget. The cap keeps a host that
    // refuses everything from looping forever: narrowing an eight-range plan
    // needs at most seven deferrals, the rest covers a slot that is still busy
    // on the host side (UploadHaven releases it after 15-20 s).
    static constexpr int kMaxDeferrals = 12;

    explicit AdaptiveConcurrency(int limit);

    int limit() const { return limit_; }

    // The host refused one more range: give up exactly one slot. When a whole
    // batch is refused at once (8 ranges against a host that allows 2), the
    // refusals arrive together and the limit lands on the host's real value in
    // the same batch. The wait for the deferred range is what stops the next
    // start from re-testing the limit immediately.
    void on_refusal();

    // A range completed: the current limit is usable and the deferral streak
    // restarts, so a long download cannot exhaust the cap by accident.
    void on_success() { deferrals_ = 0; }

    void on_deferral() { ++deferrals_; }
    int deferrals() const { return deferrals_; }
    bool exhausted() const { return deferrals_ >= kMaxDeferrals; }

    // Wait before the deferred range tries again. The refused range is the one
    // that has to give way, so the waits start at 2 s and double up to 8 s.
    static int64_t defer_backoff_ms(int deferrals);

  private:
    int limit_;
    int deferrals_ = 0;
};

}  // namespace swiftdm
