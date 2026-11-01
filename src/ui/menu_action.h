#pragma once

#include <QAction>

namespace swiftdm::ui {

// QMenu::exec returns nullptr when the menu is dismissed without a choice
// (Esc, click outside, or the window losing focus). Several context-menu
// actions are optional and only created for some rows, so their local
// QAction* stays nullptr in the other cases. A plain `chosen == action`
// comparison would then be true for "no choice" vs "action not created" and
// would run the action the user never picked (0.22.7 fixed a resume/redownload
// that way). Always route the comparison through this helper.
inline bool menu_action_matches(QAction* chosen, QAction* action) {
    return chosen != nullptr && action != nullptr && chosen == action;
}

}  // namespace swiftdm::ui
