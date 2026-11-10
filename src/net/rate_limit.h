#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>

namespace swiftdm {

// A per-download receive-rate gate shared between the UI and the network
// worker. Updating the atomic rate takes effect on the next received chunk.
//
// Pacing is a token bucket that accrues credit from the wall clock, so the
// time a chunk spends outside this call (curl's own bookkeeping, the disk
// write, scheduler wake-up latency, the platform timer granularity) is paid
// back instead of being added on top of the configured rate.
//
// The earlier implementation slept `bytes / rate` per write callback. Every
// sleep also costs a fixed amount of time that it never accounted for, so the
// achieved rate was bounded by `chunk_size / sleep_overhead` no matter how high
// the limit was set. libcurl hands out 16 KiB chunks (CURL_MAX_WRITE_SIZE), so
// on a machine where a sleep costs ~3 ms every limit above ~5 MB/s produced
// ~5 MB/s: raising the limiter stopped having any effect.
class BandwidthLimit {
  public:
    explicit BandwidthLimit(int64_t bytes_per_second = 0)
        : bytes_per_second_(std::max<int64_t>(0, bytes_per_second)),
          last_(Clock::now()) {}

    void set_bytes_per_second(int64_t value) {
        // Bank the time spent at the old rate before switching, so a live edit
        // neither loses nor invents credit.
        std::lock_guard<std::mutex> lock(mutex_);
        accrue_locked(Clock::now(), bytes_per_second());
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
        if (interrupted()) return false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const int64_t rate = bytes_per_second();
            accrue_locked(Clock::now(), rate);
            if (rate <= 0) {
                credit_ = 0.0;  // unlimited: never bank a burst for later
                return true;
            }
            credit_ -= static_cast<double>(bytes);
        }
        for (;;) {
            if (interrupted()) {
                // The chunk is dropped by the caller: hand its debt back so a
                // pause followed by a resume does not start with a penalty.
                std::lock_guard<std::mutex> lock(mutex_);
                credit_ += static_cast<double>(bytes);
                last_ = Clock::now();
                return false;
            }
            double wait_seconds = 0.0;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                const int64_t rate = bytes_per_second();
                if (rate <= 0) {
                    credit_ = 0.0;
                    return true;
                }
                accrue_locked(Clock::now(), rate);
                if (credit_ >= 0.0) return true;
                wait_seconds = -credit_ / static_cast<double>(rate);
            }
            std::this_thread::sleep_for(std::chrono::duration<double>(
                std::min(wait_seconds, kMaxSliceSeconds)));
        }
    }

  private:
    using Clock = std::chrono::steady_clock;
    // Longest single sleep, so a pause request or a new rate is noticed fast.
    static constexpr double kMaxSliceSeconds = 0.02;
    // Never bank more than this much traffic: a long idle gap must not turn
    // into an unbounded burst once the transfer resumes.
    static constexpr double kBurstSeconds = 0.05;

    void accrue_locked(Clock::time_point now, int64_t rate) const {
        const double elapsed = std::chrono::duration<double>(now - last_).count();
        last_ = now;
        if (rate <= 0) {
            credit_ = 0.0;
            return;
        }
        const double cap = static_cast<double>(rate) * kBurstSeconds;
        credit_ = std::min(credit_ + elapsed * static_cast<double>(rate), cap);
    }

    std::atomic<int64_t> bytes_per_second_{0};
    std::atomic<bool> interrupted_{false};
    mutable std::mutex mutex_;
    mutable double credit_ = 0.0;  // bytes earned minus bytes delivered
    mutable Clock::time_point last_;
};

}  // namespace swiftdm
