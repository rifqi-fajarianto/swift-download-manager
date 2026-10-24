#include "ui/queue_logic.h"

#include <algorithm>

namespace swiftdm::ui {

bool queue_item_startable(DownloadItem::State state) {
    return state == DownloadItem::State::Queued ||
           state == DownloadItem::State::Paused ||
           state == DownloadItem::State::Error;
}

bool queue_item_waiting(DownloadItem::State state) {
    return state == DownloadItem::State::Queued;
}

namespace {

bool is_active(const QueueItemView& item) {
    return item.active || item.state == DownloadItem::State::Downloading ||
           item.state == DownloadItem::State::Probing;
}

std::vector<qint64> sorted_ids(const std::vector<QueueItemView>& items, qint64 queue_id,
                               bool want_active, bool include_paused_error,
                               bool include_complete) {
    std::vector<const QueueItemView*> selected;
    for (const QueueItemView& item : items) {
        if (item.queue_id != queue_id) continue;
        const bool active = is_active(item);
        if (active != want_active) continue;
        if (!active) {
            if (item.state == DownloadItem::State::Complete) {
                if (!include_complete) continue;
            } else if (!include_paused_error &&
                       (item.state == DownloadItem::State::Paused ||
                        item.state == DownloadItem::State::Error)) {
                continue;
            }
            if (!include_complete && !queue_item_startable(item.state)) continue;
        }
        selected.push_back(&item);
    }
    std::sort(selected.begin(), selected.end(),
              [](const QueueItemView* a, const QueueItemView* b) {
                  return a->history_id < b->history_id;
              });
    std::vector<qint64> ids;
    ids.reserve(selected.size());
    for (const QueueItemView* item : selected) ids.push_back(item->history_id);
    return ids;
}

}  // namespace

std::vector<qint64> queue_start_order(const std::vector<QueueItemView>& items,
                                      qint64 queue_id, bool include_paused_error) {
    return sorted_ids(items, queue_id, false, include_paused_error, false);
}

std::vector<qint64> queue_waiting_order(const std::vector<QueueItemView>& items,
                                        qint64 queue_id) {
    std::vector<const QueueItemView*> selected;
    for (const QueueItemView& item : items) {
        if (item.queue_id != queue_id || is_active(item)) continue;
        if (!queue_item_waiting(item.state)) continue;
        selected.push_back(&item);
    }
    std::sort(selected.begin(), selected.end(),
              [](const QueueItemView* a, const QueueItemView* b) {
                  return a->history_id < b->history_id;
              });
    std::vector<qint64> ids;
    ids.reserve(selected.size());
    for (const QueueItemView* item : selected) ids.push_back(item->history_id);
    return ids;
}

std::vector<qint64> queue_active_order(const std::vector<QueueItemView>& items,
                                       qint64 queue_id) {
    return sorted_ids(items, queue_id, true, true, true);
}

int queue_active_count(const std::vector<QueueItemView>& items, qint64 queue_id) {
    int count = 0;
    for (const QueueItemView& item : items) {
        if (item.queue_id == queue_id && is_active(item)) ++count;
    }
    return count;
}

std::vector<qint64> queue_items_to_pause(const std::vector<QueueItemView>& items,
                                         qint64 queue_id, int max_simultaneous) {
    if (max_simultaneous <= 0) return {};
    const std::vector<qint64> active = queue_active_order(items, queue_id);
    if (static_cast<int>(active.size()) <= max_simultaneous) return {};
    return std::vector<qint64>(active.begin() + max_simultaneous, active.end());
}

int queue_speed_share_kbps(int total_kbps, int active_count) {
    if (total_kbps <= 0 || active_count <= 0) return 0;
    return std::max(1, total_kbps / active_count);
}

bool queue_all_complete(const std::vector<QueueItemView>& items, qint64 queue_id,
                        int* item_count) {
    int count = 0;
    for (const QueueItemView& item : items) {
        if (item.queue_id != queue_id) continue;
        ++count;
        if (item.state != DownloadItem::State::Complete) {
            if (item_count) *item_count = count;
            return false;
        }
    }
    if (item_count) *item_count = count;
    return count > 0;
}

bool uses_global_completion_actions(qint64 queue_id) { return queue_id <= 0; }

void ShutdownScheduler::arm(qint64 queue_id, int delay_seconds, qint64 now_ms) {
    if (queue_id <= 0) return;
    const int delay = std::max(0, delay_seconds);
    deadlines_[queue_id] = now_ms + static_cast<qint64>(delay) * 1000;
}

void ShutdownScheduler::cancel(qint64 queue_id) { deadlines_.erase(queue_id); }

void ShutdownScheduler::cancel_all() { deadlines_.clear(); }

bool ShutdownScheduler::pending(qint64 queue_id) const {
    return deadlines_.find(queue_id) != deadlines_.end();
}

qint64 ShutdownScheduler::deadline_ms(qint64 queue_id) const {
    const auto it = deadlines_.find(queue_id);
    return it == deadlines_.end() ? 0 : it->second;
}

int ShutdownScheduler::remaining_seconds(qint64 queue_id, qint64 now_ms) const {
    const qint64 deadline = deadline_ms(queue_id);
    if (deadline == 0) return 0;
    const qint64 remaining = deadline - now_ms;
    if (remaining <= 0) return 0;
    return static_cast<int>((remaining + 999) / 1000);
}

qint64 ShutdownScheduler::poll_due(qint64 now_ms) {
    for (auto it = deadlines_.begin(); it != deadlines_.end(); ++it) {
        if (it->second <= now_ms) {
            const qint64 id = it->first;
            deadlines_.erase(it);
            return id;
        }
    }
    return 0;
}

}  // namespace swiftdm::ui
