#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <thread>

namespace swiftdm {

// A per-download receive-rate gate shared between the UI and the network
// worker. Updating the atomic rate takes effect on the next received chunk.
class BandwidthLimit {
  public:
    explicit BandwidthLimit(int64_t bytes_per_second = 0)
        : bytes_per_second_(std::max<int64_t>(0, bytes_per_second)) {}

    void set_bytes_per_second(int64_t value) {
        bytes_per_second_.store(std::max<int64_t>(0, value), std::memory_order_relaxed);
    }

    int64_t bytes_per_second() const {
        return bytes_per_second_.load(std::memory_order_relaxed);
    }

    void interrupt() { interrupted_.store(true, std::memory_order_relaxed); }
    void clear_interrupt() { interrupted_.store(false, std::memory_order_relaxed); }
    bool interrupted() const { return interrupted_.load(std::memory_order_relaxed); }

    // Called from curl's write callback. Sleep in short slices so pause and
    // rate changes are noticed quickly. Callbacks for a given transfer are
    // serialized by its curl multi loop, so pacing here caps aggregate rate
    // across parallel ranges as well as sequential protocols.
    bool wait_for_bytes(size_t bytes) const {
        double remaining = static_cast<double>(bytes);
        auto last = std::chrono::steady_clock::now();
        while (remaining > 0.0) {
            if (interrupted()) return false;
            const int64_t rate = bytes_per_second();
            if (rate <= 0) return true;
            const double delay_seconds = remaining / static_cast<double>(rate);
            const auto slice = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double>(std::min(delay_seconds, 0.02)));
            const auto now = std::chrono::steady_clock::now();
            if (now - last < slice) {
                std::this_thread::sleep_for(slice - (now - last));
            }
            const auto after_sleep = std::chrono::steady_clock::now();
            const double elapsed = std::chrono::duration<double>(after_sleep - last).count();
            remaining -= elapsed * rate;
            last = after_sleep;
        }
        return !interrupted();
    }

  private:
    std::atomic<int64_t> bytes_per_second_{0};
    std::atomic<bool> interrupted_{false};
};

}  // namespace swiftdm
