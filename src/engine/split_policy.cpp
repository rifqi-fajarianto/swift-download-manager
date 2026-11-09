#include "engine/split_policy.h"

#include <algorithm>

namespace swiftdm {

std::optional<SplitChoice> pick_split(const std::vector<SplitView>& views, double ref_speed_bps,
                                      const SplitPolicy& policy) {
    const SplitView* best = nullptr;
    double best_eta_ms = 0.0;
    for (const SplitView& view : views) {
        if (view.streaming_ms < policy.warmup_ms) continue;
        if (view.since_split_ms < policy.cooldown_ms) continue;
        if (view.remaining < policy.min_remaining) continue;
        const double speed = std::max(view.speed_bps, policy.speed_floor_bps);
        const double eta_ms = static_cast<double>(view.remaining) / speed * 1000.0;
        // Strictly greater keeps the lowest index on a tie, which makes the
        // choice deterministic for the tests.
        if (!best || eta_ms > best_eta_ms) {
            best = &view;
            best_eta_ms = eta_ms;
        }
    }
    if (!best || best_eta_ms < static_cast<double>(policy.min_eta_ms)) return std::nullopt;

    const double v = std::max(best->speed_bps, policy.speed_floor_bps);
    const double r = std::max(ref_speed_bps, v);
    const double frac = std::clamp(v / (v + r), policy.min_keep, policy.max_keep);
    uint64_t keep = static_cast<uint64_t>(static_cast<double>(best->remaining) * frac);

    // The victim has to keep something and the thief has to get a usable range.
    if (keep == 0) keep = 1;
    if (keep >= best->remaining) keep = best->remaining - 1;
    if (best->remaining - keep < policy.min_thief) {
        if (best->remaining <= policy.min_thief) return std::nullopt;
        keep = best->remaining - policy.min_thief;
    }
    if (keep == 0 || keep >= best->remaining) return std::nullopt;
    return SplitChoice{best->index, keep};
}

}  // namespace swiftdm
