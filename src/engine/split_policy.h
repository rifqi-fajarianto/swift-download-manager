#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace swiftdm {

// Thresholds for splitting a running segment when a connection slot is free.
// The values are starting points: a real download's per-download log records
// every split, and the thresholds are tuned from that data.
struct SplitPolicy {
    uint64_t min_remaining = 2u << 20;  // below this the handshake costs more than it saves
    int64_t min_eta_ms = 4000;          // only split when the tail is worth a new connection
    int64_t warmup_ms = 1500;           // speed EMA is not trustworthy before this
    int64_t cooldown_ms = 2000;         // do not split a segment that was just split
    double min_keep = 0.2;              // smallest share the victim keeps
    double max_keep = 0.5;              // a healthy victim keeps half at most
    uint64_t min_thief = 1u << 20;      // never hand the thief less than this
    double speed_floor_bps = 1024.0;    // avoids dividing by ~0 for a stalled segment
};

// One active, unfinished segment as seen by the policy.
struct SplitView {
    size_t index = 0;         // index into TransferState::segments
    uint64_t remaining = 0;   // length() - done
    double speed_bps = 0;     // per-segment EMA
    int64_t streaming_ms = -1;  // ms since the first body byte; < 0 = not streaming yet
    int64_t since_split_ms = 0;  // ms since this segment was created by a split
};

struct SplitChoice {
    size_t index = 0;
    uint64_t keep = 0;  // bytes the victim keeps; 0 < keep < remaining
};

// Picks the active segment with the largest estimated remaining time, or
// nullopt when nothing is worth splitting. Pure function: no I/O, no clock.
std::optional<SplitChoice> pick_split(const std::vector<SplitView>& views,
                                      double ref_speed_bps, const SplitPolicy& policy = {});

}  // namespace swiftdm
