// Headless check of the toolbar Resume/Stop availability rules, including a
// mixed selection. No GUI is created, so this stays within the CLI/backend-only
// test policy (AGENTS.md §6).

#include <cstdio>

#include "ui/download_model.h"

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

using swiftdm::ui::DownloadItem;
using swiftdm::ui::ToolbarActionState;

struct Case {
    const char* name;
    DownloadItem::State state;
    bool has_task;
    bool task_active;
    bool resume;
    bool stop;
};

const Case kCases[] = {
    {"a downloading row can only be stopped", DownloadItem::State::Downloading, true, true,
     false, true},
    {"a paused row can only be resumed", DownloadItem::State::Paused, true, false, true,
     false},
    {"a failed row can only be resumed", DownloadItem::State::Error, true, false, true,
     false},
    {"a queued inactive row can only be resumed", DownloadItem::State::Queued, true, false,
     true, false},
    {"a complete row cannot be resumed or stopped", DownloadItem::State::Complete, true,
     false, false, false},
    {"a probing row cannot be resumed or stopped", DownloadItem::State::Probing, true, true,
     false, false},
    {"a downloading row without a worker is not actionable", DownloadItem::State::Downloading,
     true, false, false, false},
    {"a paused row with a running worker is not actionable", DownloadItem::State::Paused,
     true, true, false, false},
    {"a paused row without a task is not actionable", DownloadItem::State::Paused, false,
     false, false, false},
    {"a downloading row without a task is not actionable",
     DownloadItem::State::Downloading, false, true, false, false},
};

void merge(ToolbarActionState* actions, DownloadItem::State state, bool has_task,
           bool task_active) {
    swiftdm::ui::merge_toolbar_action_state(*actions, state, has_task, task_active);
}

}  // namespace

int main() {
    for (const Case& test : kCases) {
        check(swiftdm::ui::item_can_resume(test.state, test.has_task, test.task_active) ==
                  test.resume,
              test.name);
        check(swiftdm::ui::item_can_stop(test.state, test.has_task, test.task_active) ==
                  test.stop,
              test.name);
    }

    // A mixed selection enables each action when at least one row can use it.
    ToolbarActionState mixed;
    check(!mixed.resume && !mixed.stop, "an empty selection disables both actions");
    merge(&mixed, DownloadItem::State::Downloading, true, true);
    merge(&mixed, DownloadItem::State::Paused, true, false);
    check(mixed.resume, "downloading plus paused enables Resume");
    check(mixed.stop, "downloading plus paused enables Stop");

    ToolbarActionState resumable;
    merge(&resumable, DownloadItem::State::Complete, true, false);
    merge(&resumable, DownloadItem::State::Error, true, false);
    check(resumable.resume, "complete plus failed enables Resume");
    check(!resumable.stop, "complete plus failed leaves Stop disabled");

    ToolbarActionState complete_only;
    merge(&complete_only, DownloadItem::State::Complete, true, false);
    merge(&complete_only, DownloadItem::State::Complete, true, false);
    check(!complete_only.resume && !complete_only.stop,
          "complete rows disable both actions");

    if (failures == 0) std::printf("action_state: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
