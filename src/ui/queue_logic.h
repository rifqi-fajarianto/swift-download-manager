#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

#include "ui/download_model.h"

namespace swiftdm::ui {

// A queue item as the scheduler sees it. Keeping this independent of
// DownloadTask lets the ordering and limit rules be tested without a transfer.
struct QueueItemView {
    qint64 history_id = 0;
    qint64 queue_id = 0;
    DownloadItem::State state = DownloadItem::State::Queued;
    bool active = false;
};

// Rows that Start Queue may resume: queued, manually paused and failed rows are
// all retried. Complete and actively running rows are skipped.
bool queue_item_startable(DownloadItem::State state);

// Only waiting rows are picked up by the automatic scheduler. Paused and
// failed rows wait for an explicit Start Queue (or a manual Start).
bool queue_item_waiting(DownloadItem::State state);

std::vector<qint64> queue_start_order(const std::vector<QueueItemView>& items,
                                      qint64 queue_id, bool include_paused_error);
std::vector<qint64> queue_waiting_order(const std::vector<QueueItemView>& items,
                                        qint64 queue_id);
std::vector<qint64> queue_active_order(const std::vector<QueueItemView>& items,
                                       qint64 queue_id);
int queue_active_count(const std::vector<QueueItemView>& items, qint64 queue_id);

// Active rows to pause after the limit was lowered. The earliest rows keep
// running; the returned ids are the latest ones beyond `max_simultaneous`.
// `0` means unlimited and therefore returns nothing.
std::vector<qint64> queue_items_to_pause(const std::vector<QueueItemView>& items,
                                         qint64 queue_id, int max_simultaneous);

// Splits the queue-wide budget across the active rows. A zero total means
// unlimited; at least 1 KB/s is assigned when a positive budget exists.
int queue_speed_share_kbps(int total_kbps, int active_count);

// True when the queue has at least one row and every row is complete.
bool queue_all_complete(const std::vector<QueueItemView>& items, qint64 queue_id,
                        int* item_count = nullptr);

// Global completion actions (the Download Complete window and "open the
// download folder when a download completes") belong to the main Downloads
// list. A queue owns its completion notice through notify_item, so queue items
// skip both actions.
bool uses_global_completion_actions(qint64 queue_id);

// Countdown state machine for the per-queue shutdown option. It uses an
// explicit millisecond clock so the countdown and cancellation rules can be
// tested without timers or powering off a machine.
class ShutdownScheduler {
  public:
    void arm(qint64 queue_id, int delay_seconds, qint64 now_ms);
    void cancel(qint64 queue_id);
    void cancel_all();
    bool pending(qint64 queue_id) const;
    qint64 deadline_ms(qint64 queue_id) const;
    int remaining_seconds(qint64 queue_id, qint64 now_ms) const;
    // Returns and clears the queue whose deadline has passed, or 0.
    qint64 poll_due(qint64 now_ms);
    std::size_t pending_count() const { return deadlines_.size(); }

  private:
    std::map<qint64, qint64> deadlines_;
};

}  // namespace swiftdm::ui
