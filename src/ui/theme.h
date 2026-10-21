#pragma once

#include <QString>

class QApplication;

namespace swiftdm::ui {

// App theme. "System" follows the desktop color scheme.
enum class ThemeMode {
    System,
    Light,
    Dark,
};

// The theme that is currently applied (true = dark). Kept in the header so
// the icon and model code can adapt their colors without linking this module.
inline bool& dark_theme_state() {
    static bool dark = true;
    return dark;
}

inline bool is_dark_theme() { return dark_theme_state(); }

// Maps the stored setting value ("system", "light", "dark") to a mode; an
// unknown value falls back to Dark.
ThemeMode theme_mode_from_string(const QString& value);
QString theme_mode_to_string(ThemeMode mode);

// Resolves "System" against the current desktop color scheme.
ThemeMode effective_theme(ThemeMode mode);

// Applies the theme (Fusion style + palette) to the application. The Fusion
// style is used for both themes so the widgets look the same everywhere; the
// difference between the original's custom theme and this one is documented
// in 06-parity-checklist.md.
void apply_theme(QApplication& app, ThemeMode mode);

}  // namespace swiftdm::ui
