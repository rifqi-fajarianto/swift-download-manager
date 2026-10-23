// CLI/backend test for the queue scheduling rules: start/wait ordering, limit
// pauses, speed sharing, all-complete detection and the shutdown countdown.
// Nothing here creates a window (see AGENTS.md §6).

#include <cstdio>
#include <vector>

#include "ui/queue_logic.h"

namespace {

int failures = 0;

void check(bool condition, const char* description) {
    if (!condition) {
        std::printf("FAIL %s\n", description);
        ++failures;
    }
}

using swiftdm::ui::DownloadItem;
using swiftdm::ui::QueueItemView;
using swiftdm::ui::ShutdownScheduler;

QueueItemView make_item(qint64 history_id, qint64 queue_id,
                        DownloadItem::State state, bool active = false) {
    QueueItemView item;
    item.history_id = history_id;
    item.queue_id = queue_id;
    item.state = state;
    item.active = active;
    return item;
}

bool same_ids(const std::vector<qint64>& actual, const std::vector<qint64>& expected) {
    return actual == expected;
}

}  // namespace

int main() {
    using swiftdm::ui::queue_active_count;
    using swiftdm::ui::queue_active_order;
    using swiftdm::ui::queue_all_complete;
    using swiftdm::ui::queue_item_startable;
    using swiftdm::ui::queue_item_waiting;
    using swiftdm::ui::queue_items_to_pause;
    using swiftdm::ui::queue_speed_share_kbps;
    using swiftdm::ui::queue_start_order;
    using swiftdm::ui::queue_waiting_order;

    check(queue_item_startable(DownloadItem::State::Queued) &&
              queue_item_startable(DownloadItem::State::Paused) &&
              queue_item_startable(DownloadItem::State::Error),
          "queued, paused and failed rows are startable");
    check(!queue_item_startable(DownloadItem::State::Downloading) &&
              !queue_item_startable(DownloadItem::State::Probing) &&
              !queue_item_startable(DownloadItem::State::Complete),
          "active and complete rows are not startable");
    check(queue_item_waiting(DownloadItem::State::Queued) &&
              !queue_item_waiting(DownloadItem::State::Paused) &&
              !queue_item_waiting(DownloadItem::State::Error),
          "the automatic scheduler only picks waiting rows");

    const std::vector<QueueItemView> items = {
        make_item(101, 7, DownloadItem::State::Complete),
        make_item(102, 7, DownloadItem::State::Paused),
        make_item(103, 7, DownloadItem::State::Queued),
        make_item(104, 7, DownloadItem::State::Error),
        make_item(105, 7, DownloadItem::State::Downloading, true),
        make_item(106, 7, DownloadItem::State::Queued, true),
        make_item(107, 7, DownloadItem::State::Queued),
        make_item(108, 8, DownloadItem::State::Queued),
    };

    check(same_ids(queue_start_order(items, 7, true), {102, 103, 104, 107}),
          "Start Queue uses insertion order and includes paused/failed rows");
    check(same_ids(queue_start_order(items, 7, false), {103, 107}),
          "without paused/failed rows Start Queue only resumes queued rows");
    check(same_ids(queue_waiting_order(items, 7), {103, 107}),
          "the scheduler ignores paused, failed and active rows");
    check(same_ids(queue_active_order(items, 7), {105, 106}),
          "active rows are ordered by insertion id");
    check(queue_active_count(items, 7) == 2, "the active count only counts this queue");
    check(queue_active_count(items, 8) == 0, "another queue has no active rows");

    check(same_ids(queue_items_to_pause(items, 7, 1), {106}),
          "lowering the limit to one pauses the latest active row");
    check(same_ids(queue_items_to_pause(items, 7, 2), {}),
          "two active rows fit under a limit of two");
    check(same_ids(queue_items_to_pause(items, 7, 0), {}),
          "zero means unlimited and pauses nothing");

    check(queue_speed_share_kbps(0, 3) == 0, "a zero total means unlimited");
    check(queue_speed_share_kbps(100, 3) == 33, "the total is split across active rows");
    check(queue_speed_share_kbps(1, 2) == 1, "a tiny share stays at least 1 KB/s");
    check(queue_speed_share_kbps(1024, 4) == 256, "the split uses whole KB/s");
    check(queue_speed_share_kbps(100, 0) == 0, "no active row means no share");

    int count = 0;
    check(queue_all_complete({make_item(201, 9, DownloadItem::State::Complete)}, 9, &count) &&
              count == 1,
          "a queue with one completed row is complete");
    check(!queue_all_complete({make_item(201, 9, DownloadItem::State::Complete),
                               make_item(202, 9, DownloadItem::State::Queued)},
                              9, &count),
          "a queued row keeps the queue incomplete");
    check(!queue_all_complete({}, 9, &count), "an empty queue is never complete");
    check(!queue_all_complete(items, 9, &count),
          "rows from another queue do not make this queue complete");
    check(queue_all_complete({make_item(301, 10, DownloadItem::State::Complete),
                              make_item(302, 10, DownloadItem::State::Complete)},
                             10, &count) &&
              count == 2,
          "all completed rows make the queue complete and report the count");

    ShutdownScheduler shutdown;
    check(!shutdown.pending(7), "a fresh scheduler has no pending shutdown");
    shutdown.arm(7, 10, 1000);
    check(shutdown.pending(7) && shutdown.deadline_ms(7) == 11000,
          "arming stores the deadline");
    check(shutdown.remaining_seconds(7, 1000) == 10,
          "the remaining time starts at the configured delay");
    check(shutdown.remaining_seconds(7, 10500) == 1,
          "a partial second is rounded up");
    check(shutdown.poll_due(10999) == 0 && shutdown.pending(7),
          "the deadline has not passed yet");
    check(shutdown.poll_due(11000) == 7 && !shutdown.pending(7),
          "a due deadline is returned and cleared");
    check(shutdown.remaining_seconds(7, 11000) == 0,
          "a cleared deadline has no remaining time");

    shutdown.arm(7, 0, 5000);
    check(shutdown.poll_due(5000) == 7, "a zero delay is due immediately");
    shutdown.arm(0, 5, 0);
    check(!shutdown.pending(0), "an invalid queue id cannot arm a shutdown");
    shutdown.arm(7, -5, 1000);
    check(shutdown.deadline_ms(7) == 1000,
          "a negative delay is clamped to zero seconds");

    shutdown.arm(7, 60, 0);
    shutdown.arm(8, 60, 0);
    check(shutdown.pending_count() == 2, "the scheduler can track multiple deadlines");
    shutdown.cancel(7);
    check(!shutdown.pending(7) && shutdown.pending(8),
          "cancelling one countdown leaves the other alone");
    shutdown.cancel_all();
    check(shutdown.pending_count() == 0, "cancel_all clears every countdown");

    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("queue_logic tests passed\n");
    return 0;
}
