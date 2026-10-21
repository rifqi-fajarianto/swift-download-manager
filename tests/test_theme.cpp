// Headless check of the theme module: the setting value is mapped to a mode,
// the light and the dark palette differ, and the "follow system" mode always
// resolves to a concrete theme. No window is shown.

#include <QApplication>
#include <QPalette>
#include <QString>

#include <cstdio>

#include "ui/theme.h"

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

    using swiftdm::ui::ThemeMode;

    check(swiftdm::ui::theme_mode_from_string(QStringLiteral("light")) == ThemeMode::Light,
          "\"light\" maps to the light theme");
    check(swiftdm::ui::theme_mode_from_string(QStringLiteral("dark")) == ThemeMode::Dark,
          "\"dark\" maps to the dark theme");
    check(swiftdm::ui::theme_mode_from_string(QStringLiteral("system")) == ThemeMode::System,
          "\"system\" maps to the system theme");
    check(swiftdm::ui::theme_mode_from_string(QStringLiteral(" SYSTEM ")) == ThemeMode::System,
          "the value is matched regardless of case and spaces");
    check(swiftdm::ui::theme_mode_from_string(QStringLiteral("neon")) == ThemeMode::Dark,
          "an unknown value falls back to the dark theme");

    check(swiftdm::ui::theme_mode_to_string(ThemeMode::Light) == QStringLiteral("light"),
          "the light mode is stored as \"light\"");
    check(swiftdm::ui::theme_mode_to_string(ThemeMode::Dark) == QStringLiteral("dark"),
          "the dark mode is stored as \"dark\"");
    check(swiftdm::ui::theme_mode_to_string(ThemeMode::System) == QStringLiteral("system"),
          "the system mode is stored as \"system\"");

    // The system mode always resolves to a concrete theme.
    const ThemeMode resolved = swiftdm::ui::effective_theme(ThemeMode::System);
    check(resolved == ThemeMode::Light || resolved == ThemeMode::Dark,
          "the system theme resolves to light or dark");
    check(swiftdm::ui::effective_theme(ThemeMode::Light) == ThemeMode::Light,
          "the light theme stays light");
    check(swiftdm::ui::effective_theme(ThemeMode::Dark) == ThemeMode::Dark,
          "the dark theme stays dark");

    swiftdm::ui::apply_theme(app, ThemeMode::Dark);
    const QColor dark_window = app.palette().color(QPalette::Window);
    const QColor dark_text = app.palette().color(QPalette::WindowText);
    check(swiftdm::ui::is_dark_theme(), "the dark theme is reported as dark");
    check(dark_window.lightness() < 128, "the dark theme uses a dark window color");

    swiftdm::ui::apply_theme(app, ThemeMode::Light);
    const QColor light_window = app.palette().color(QPalette::Window);
    const QColor light_text = app.palette().color(QPalette::WindowText);
    check(!swiftdm::ui::is_dark_theme(), "the light theme is not reported as dark");
    check(light_window.lightness() > 128, "the light theme uses a light window color");
    check(dark_window != light_window, "the two themes use different window colors");
    check(dark_text != light_text, "the two themes use different text colors");

    if (failures == 0) std::printf("theme: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
