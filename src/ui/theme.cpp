#include "ui/theme.h"

#include <QApplication>
#include <QGuiApplication>
#include <QPalette>
#include <QStyleFactory>
#include <QStyleHints>

namespace swiftdm::ui {

namespace {

QPalette dark_palette() {
    QPalette p;
    p.setColor(QPalette::Window, QColor(0x2b, 0x2b, 0x2b));
    p.setColor(QPalette::WindowText, QColor(0xe8, 0xe8, 0xe8));
    p.setColor(QPalette::Base, QColor(0x24, 0x24, 0x24));
    p.setColor(QPalette::AlternateBase, QColor(0x2b, 0x2b, 0x2b));
    p.setColor(QPalette::ToolTipBase, QColor(0x1e, 0x1e, 0x1e));
    p.setColor(QPalette::ToolTipText, QColor(0xe8, 0xe8, 0xe8));
    p.setColor(QPalette::Text, QColor(0xe8, 0xe8, 0xe8));
    p.setColor(QPalette::Button, QColor(0x38, 0x38, 0x38));
    p.setColor(QPalette::ButtonText, QColor(0xe8, 0xe8, 0xe8));
    p.setColor(QPalette::BrightText, QColor(0xff, 0x5a, 0x5a));
    p.setColor(QPalette::Link, QColor(0x4d, 0xa3, 0xff));
    p.setColor(QPalette::Highlight, QColor(0x2f, 0x6f, 0xb5));
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::PlaceholderText, QColor(0x8a, 0x8a, 0x8a));
    p.setColor(QPalette::Disabled, QPalette::Text, QColor(0x7c, 0x7c, 0x7c));
    p.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(0x7c, 0x7c, 0x7c));
    p.setColor(QPalette::Disabled, QPalette::WindowText, QColor(0x7c, 0x7c, 0x7c));
    return p;
}

QPalette light_palette() {
    QPalette p;
    p.setColor(QPalette::Window, QColor(0xf0, 0xf0, 0xf0));
    p.setColor(QPalette::WindowText, QColor(0x1e, 0x1e, 0x1e));
    p.setColor(QPalette::Base, QColor(0xff, 0xff, 0xff));
    p.setColor(QPalette::AlternateBase, QColor(0xf6, 0xf6, 0xf6));
    p.setColor(QPalette::ToolTipBase, QColor(0xff, 0xff, 0xff));
    p.setColor(QPalette::ToolTipText, QColor(0x1e, 0x1e, 0x1e));
    p.setColor(QPalette::Text, QColor(0x1e, 0x1e, 0x1e));
    p.setColor(QPalette::Button, QColor(0xe4, 0xe4, 0xe4));
    p.setColor(QPalette::ButtonText, QColor(0x1e, 0x1e, 0x1e));
    p.setColor(QPalette::BrightText, QColor(0xc0, 0x30, 0x30));
    p.setColor(QPalette::Link, QColor(0x1a, 0x6f, 0xb5));
    p.setColor(QPalette::Highlight, QColor(0x2f, 0x6f, 0xb5));
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::PlaceholderText, QColor(0x77, 0x77, 0x77));
    p.setColor(QPalette::Disabled, QPalette::Text, QColor(0x9a, 0x9a, 0x9a));
    p.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(0x9a, 0x9a, 0x9a));
    p.setColor(QPalette::Disabled, QPalette::WindowText, QColor(0x9a, 0x9a, 0x9a));
    return p;
}

// True when the desktop asks for a dark color scheme. The style hints are
// authoritative; the palette is only a fallback for platforms that do not
// report a color scheme.
bool system_prefers_dark() {
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    if (QGuiApplication::instance()) {
        const QStyleHints* hints = QGuiApplication::styleHints();
        if (hints && hints->colorScheme() != Qt::ColorScheme::Unknown) {
            return hints->colorScheme() == Qt::ColorScheme::Dark;
        }
    }
#endif
    // The fallback reads the platform palette, so it is captured once: after
    // the app applied its own palette the value would no longer be the
    // desktop's.
    static const bool fallback = [] {
        return QApplication::palette().color(QPalette::Window).lightness() < 128;
    }();
    return fallback;
}

}  // namespace

ThemeMode theme_mode_from_string(const QString& value) {
    const QString v = value.trimmed().toLower();
    if (v == QStringLiteral("system")) return ThemeMode::System;
    if (v == QStringLiteral("light")) return ThemeMode::Light;
    return ThemeMode::Dark;
}

QString theme_mode_to_string(ThemeMode mode) {
    switch (mode) {
        case ThemeMode::System:
            return QStringLiteral("system");
        case ThemeMode::Light:
            return QStringLiteral("light");
        case ThemeMode::Dark:
            break;
    }
    return QStringLiteral("dark");
}

ThemeMode effective_theme(ThemeMode mode) {
    if (mode != ThemeMode::System) return mode;
    return system_prefers_dark() ? ThemeMode::Dark : ThemeMode::Light;
}

void apply_theme(QApplication& app, ThemeMode mode) {
    const ThemeMode resolved = effective_theme(mode);
    dark_theme_state() = resolved == ThemeMode::Dark;

    if (QStyleFactory::keys().contains("Fusion")) {
        app.setStyle(QStyleFactory::create("Fusion"));
    }
    app.setPalette(resolved == ThemeMode::Dark ? dark_palette() : light_palette());
}

}  // namespace swiftdm::ui
