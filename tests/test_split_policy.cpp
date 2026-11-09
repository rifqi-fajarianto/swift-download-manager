// Unit tests for the dynamic-split victim policy. Pure logic, no network.

#include <cstdint>
#include <cstdio>
#include <vector>

#include "engine/split_policy.h"
#include "test_util.h"

namespace {

using swiftdm::pick_split;
using swiftdm::SplitChoice;
using swiftdm::SplitPolicy;
using swiftdm::SplitView;

SplitView view(size_t index, uint64_t remaining, double speed, int64_t streaming_ms = 5000,
               int64_t since_split_ms = 10000) {
    return SplitView{index, remaining, speed, streaming_ms, since_split_ms};
}

void test_ignores_segment_not_streaming() {
    const std::vector<SplitView> views{view(0, 8u << 20, 1000, /*streaming_ms=*/-1)};
    CHECK(!pick_split(views, 1000).has_value());
}

void test_ignores_warmup() {
    const SplitPolicy policy;
    const std::vector<SplitView> views{
        view(0, 8u << 20, 1000, policy.warmup_ms - 1)};
    CHECK(!pick_split(views, 1000, policy).has_value());
}

void test_ignores_cooldown() {
    const SplitPolicy policy;
    const std::vector<SplitView> views{
        view(0, 8u << 20, 1000, 5000, policy.cooldown_ms - 1)};
    CHECK(!pick_split(views, 1000, policy).has_value());
}

void test_ignores_small_remaining() {
    const SplitPolicy policy;
    const std::vector<SplitView> views{view(0, policy.min_remaining - 1, 1000)};
    CHECK(!pick_split(views, 1000, policy).has_value());
}

void test_ignores_short_eta() {
    const SplitPolicy policy;
    // 2 MiB at 100 MB/s is well under min_eta_ms.
    const std::vector<SplitView> views{view(0, 2u << 20, 100.0 * 1024 * 1024)};
    CHECK(!pick_split(views, 1000, policy).has_value());
}

void test_picks_largest_eta() {
    const std::vector<SplitView> views{
        view(0, 8u << 20, 4u << 20),  // 2 s
        view(1, 8u << 20, 1u << 20),  // 8 s  <- slowest
        view(2, 8u << 20, 2u << 20),  // 4 s
    };
    const auto choice = pick_split(views, 4u << 20);
    CHECK(choice.has_value());
    CHECK_EQ(choice->index, size_t(1));
}

void test_tie_picks_lowest_index() {
    const std::vector<SplitView> views{
        view(3, 8u << 20, 1u << 20),
        view(5, 8u << 20, 1u << 20),
    };
    const auto choice = pick_split(views, 1u << 20);
    CHECK(choice.has_value());
    CHECK_EQ(choice->index, size_t(3));
}

void test_slow_victim_keeps_small_share() {
    const SplitPolicy policy;
    const uint64_t remaining = 8u << 20;
    // Victim 100x slower than the reference: it keeps the smallest allowed share.
    const std::vector<SplitView> views{view(0, remaining, 1u << 20)};
    const auto choice = pick_split(views, 100u << 20, policy);
    CHECK(choice.has_value());
    const double frac = static_cast<double>(choice->keep) / static_cast<double>(remaining);
    CHECK(frac >= policy.min_keep - 0.01);
    CHECK(frac <= policy.min_keep + 0.01);
}

void test_equal_speed_keeps_half() {
    const SplitPolicy policy;
    const uint64_t remaining = 8u << 20;
    const std::vector<SplitView> views{view(0, remaining, 2u << 20)};
    const auto choice = pick_split(views, 2u << 20, policy);
    CHECK(choice.has_value());
    const double frac = static_cast<double>(choice->keep) / static_cast<double>(remaining);
    CHECK(frac > 0.49);
    CHECK(frac < 0.51);
}

void test_choice_invariants_over_random_input() {
    const SplitPolicy policy;
    uint64_t seed = 0x2545F4914F6CDD1Dull;
    auto next = [&seed] {
        seed ^= seed << 13;
        seed ^= seed >> 7;
        seed ^= seed << 17;
        return seed;
    };
    for (int i = 0; i < 1000; ++i) {
        const uint64_t remaining = next() % (32u << 20);
        const double speed = static_cast<double>(next() % (64u << 20));
        const double ref = static_cast<double>(next() % (64u << 20));
        const int64_t streaming = static_cast<int64_t>(next() % 10000);
        const std::vector<SplitView> views{view(0, remaining, speed, streaming)};
        const auto choice = pick_split(views, ref, policy);
        if (!choice) continue;
        CHECK(choice->keep > 0);
        CHECK(choice->keep < remaining);
        CHECK(remaining - choice->keep >= policy.min_thief);
    }
}

void test_disabled_split_when_remaining_is_min_thief() {
    const SplitPolicy policy;
    // Remaining above min_remaining but not enough for a thief plus a victim.
    const uint64_t remaining = policy.min_thief + 1;
    const std::vector<SplitView> views{view(0, remaining, 1024.0)};
    const auto choice = pick_split(views, 1024.0, policy);
    if (choice) {
        CHECK(choice->keep > 0);
        CHECK(remaining - choice->keep >= policy.min_thief);
    }
}

}  // namespace

int main() {
    test_ignores_segment_not_streaming();
    test_ignores_warmup();
    test_ignores_cooldown();
    test_ignores_small_remaining();
    test_ignores_short_eta();
    test_picks_largest_eta();
    test_tie_picks_lowest_index();
    test_slow_victim_keeps_small_share();
    test_equal_speed_keeps_half();
    test_choice_invariants_over_random_input();
    test_disabled_split_when_remaining_is_min_thief();
    return testutil::finish("test_split_policy");
}
