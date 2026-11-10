// Unit tests for the receive-rate gate (src/net/rate_limit.h).
//
// Regression cover for the "speed limiter caps at ~5 MB/s" bug: the old
// implementation slept `bytes / rate` per curl write callback and never paid
// back the sleep overhead, so the achieved rate saturated at
// `chunk_size / sleep_overhead` (~5 MB/s with libcurl's 16 KiB chunks) no
// matter how high the limit was configured. These tests assert that the
// achieved rate tracks the configured value instead of saturating.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>

#include "net/rate_limit.h"
#include "test_util.h"

namespace {

using swiftdm::BandwidthLimit;
using Clock = std::chrono::steady_clock;

constexpr double kMiB = 1024.0 * 1024.0;

double elapsed_seconds(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}

// Feeds `chunk` bytes at a time through the gate for `seconds` and returns the
// achieved rate in bytes/s. `chunk` is a parameter because the bug scaled with
// the write-callback size.
double measure(int64_t rate, size_t chunk, double seconds) {
    BandwidthLimit limit(rate);
    const auto start = Clock::now();
    uint64_t bytes = 0;
    while (elapsed_seconds(start) < seconds) {
        if (!limit.wait_for_bytes(chunk)) break;
        bytes += chunk;
    }
    const double elapsed = elapsed_seconds(start);
    return elapsed > 0.0 ? static_cast<double>(bytes) / elapsed : 0.0;
}

// Same measurement on an already-configured gate (for live-change cases).
double measure_from(BandwidthLimit& limit, size_t chunk, double seconds) {
    const auto start = Clock::now();
    uint64_t bytes = 0;
    while (elapsed_seconds(start) < seconds) {
        if (!limit.wait_for_bytes(chunk)) break;
        bytes += chunk;
    }
    const double elapsed = elapsed_seconds(start);
    return elapsed > 0.0 ? static_cast<double>(bytes) / elapsed : 0.0;
}

// The heart of the bug. A 1 KiB callback with a 20 MB/s limit used to yield
// ~9.6 MB/s; the gate must now deliver close to the configured rate.
void test_high_rate_is_not_capped_by_callback_overhead() {
    constexpr int64_t kRate = 20 * static_cast<int64_t>(kMiB);
    const double achieved = measure(kRate, 1024, 1.0);
    CHECK(achieved > 0.9 * static_cast<double>(kRate));
}

// Raising the limit above the old saturation point must keep working: the
// achieved rate has to grow with the configured value, not flatten out. Uses a
// small callback size, where the old implementation saturated first.
void test_rate_keeps_scaling_past_the_old_ceiling() {
    const double slow = measure(20 * static_cast<int64_t>(kMiB), 1024, 0.8);
    const double fast = measure(80 * static_cast<int64_t>(kMiB), 1024, 0.8);
    // Old implementation: ~9.7 MB/s vs ~15.1 MB/s (ratio ~1.6). A working gate
    // quadruples the rate because the limit itself quadrupled.
    CHECK(fast > 3.0 * slow);
}

// A configured rate must not be exceeded by a meaningful margin, otherwise the
// limiter would be useless in the other direction.
void test_does_not_overshoot_the_configured_rate() {
    constexpr int64_t kRate = 8 * static_cast<int64_t>(kMiB);
    const double achieved = measure(kRate, 16384, 1.0);
    CHECK(achieved <= 1.1 * static_cast<double>(kRate));
}

// Small rates are paced by long sleeps; they must still be exact.
void test_low_rate_stays_accurate() {
    constexpr int64_t kRate = 8 * 1024;  // 8 KB/s
    const double achieved = measure(kRate, 16384, 1.2);
    CHECK(achieved > 0.9 * static_cast<double>(kRate));
    CHECK(achieved < 1.1 * static_cast<double>(kRate));
}

// Zero means unlimited: the gate must not throttle at all.
void test_zero_rate_is_unlimited() {
    const double achieved = measure(0, 65536, 0.3);
    CHECK(achieved > 100.0 * kMiB);
}

// A pause/cancel must break out of a pacing sleep quickly, and the chunk that
// was never delivered must not be charged to the next run.
void test_interrupt_returns_promptly() {
    BandwidthLimit limit(4 * 1024);  // 16 KiB chunk would sleep ~4 s
    std::thread interruptor([&limit] {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        limit.interrupt();
    });
    const auto start = Clock::now();
    const bool delivered = limit.wait_for_bytes(16384);
    const double latency = elapsed_seconds(start);
    interruptor.join();
    CHECK(!delivered);
    CHECK(latency < 0.25);

    // After clearing the interrupt the gate works again, and the dropped chunk
    // is not treated as delivered data.
    limit.clear_interrupt();
    limit.set_bytes_per_second(0);
    CHECK(limit.wait_for_bytes(16384));
}

// A live rate change (the Options tab applies one while downloading) must take
// effect without restarting the transfer.
void test_live_rate_change_takes_effect() {
    BandwidthLimit limit(32 * 1024);
    std::thread editor([&limit] {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        limit.set_bytes_per_second(static_cast<int64_t>(4 * kMiB));
    });
    const auto start = Clock::now();
    uint64_t bytes = 0;
    while (elapsed_seconds(start) < 0.9) {
        if (!limit.wait_for_bytes(16384)) break;
        bytes += 16384;
    }
    const double achieved = static_cast<double>(bytes) / elapsed_seconds(start);
    editor.join();
    // Most of the window runs at the raised rate, so the average must be far
    // above the original 32 KB/s.
    CHECK(achieved > 512.0 * 1024.0);
}

// Going from unlimited to limited must not release a burst of banked credit.
void test_enabling_the_limiter_does_not_burst() {
    BandwidthLimit limit(0);
    for (int i = 0; i < 100; ++i) CHECK(limit.wait_for_bytes(65536));
    limit.set_bytes_per_second(static_cast<int64_t>(kMiB));
    const double achieved = measure_from(limit, 16384, 0.8);
    CHECK(achieved > 0.8 * kMiB);
    CHECK(achieved < 1.2 * kMiB);
}

}  // namespace

int main() {
    test_high_rate_is_not_capped_by_callback_overhead();
    test_rate_keeps_scaling_past_the_old_ceiling();
    test_does_not_overshoot_the_configured_rate();
    test_low_rate_stays_accurate();
    test_zero_rate_is_unlimited();
    test_interrupt_returns_promptly();
    test_live_rate_change_takes_effect();
    test_enabling_the_limiter_does_not_burst();
    return testutil::finish("test_rate_limit");
}
