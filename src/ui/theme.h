#pragma once

class QApplication;

namespace swiftdm::ui {

// Applies the dark Fusion theme used across SwiftDM windows (divergence from
// the original's custom theme is documented in 06-parity-checklist.md).
void apply_dark_theme(QApplication& app);

}  // namespace swiftdm::ui
