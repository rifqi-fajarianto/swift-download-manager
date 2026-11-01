// Headless check of the context-menu dispatch guard. QMenu::exec returns
// nullptr when the user dismisses the menu without choosing anything, and
// optional actions are nullptr for rows that do not offer them. Comparing them
// directly made "no choice" match "action not created", which resumed paused
// rows and re-downloaded completed ones (fixed in 0.22.7). No GUI is created,
// so this stays within the CLI/backend-only test policy (AGENTS.md §6).

#include <cstdio>

#include <QAction>
#include <QApplication>

#include "ui/menu_action.h"

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);

    using swiftdm::ui::menu_action_matches;

    // The reported bug: dismissing the menu (nullptr) must not match an action
    // the row never created (nullptr).
    check(!menu_action_matches(nullptr, nullptr),
          "dismissed menu must not match a missing action");

    QAction real_action(nullptr);
    QAction other_action(nullptr);

    check(!menu_action_matches(nullptr, &real_action),
          "dismissed menu must not match an existing action");
    check(!menu_action_matches(&real_action, nullptr),
          "an action must not match a missing action");
    check(menu_action_matches(&real_action, &real_action),
          "the chosen action matches itself");
    check(!menu_action_matches(&other_action, &real_action),
          "a different action does not match");
    check(!menu_action_matches(&real_action, &other_action),
          "the comparison is not symmetric by accident");

    if (failures == 0) {
        std::printf("menu_action: all checks passed\n");
    }
    return failures == 0 ? 0 : 1;
}
