#pragma once

#include <QColor>
#include <QIcon>
#include <QPixmap>
#include <QString>

namespace swiftdm::ui {

namespace colors {

// Toolbar/action tints matching the reference layout: blue for New/Resume/Stop,
// red for Delete, neutral for the rest.
extern const QColor kActionBlue;
extern const QColor kDanger;
extern const QColor kNeutral;
extern const QColor kOkGreen;

// The neutral tint follows the theme: a light gray disappears on a light
// background, so the tint is darker there.
QColor neutral_tint();

// Readable danger red for text and glyphs on the dialog background (darker in
// the light theme so the warning stays legible).
QColor danger_tint();

}  // namespace colors

// Style of the system tray icon.
enum class TrayIconStyle {
    Colorful,  // the colored browser-integration mark
    Simple,    // the monochrome mark
};

// Maps the stored setting value ("colorful", "simple") to a style; an unknown
// value falls back to Colorful.
TrayIconStyle tray_icon_style_from_string(const QString& value);

// Loads an SVG from a Qt resource path and re-colors it to a single tint. The
// SVG sources use stroke="currentColor" (rendered black by the image plugin),
// so the alpha shape is used as a monochrome mask.
QIcon tinted_icon(const QString& resource_path, const QColor& color);

// Application/window icon (colored, ui-assets/app-icon/main-icon.svg).
QIcon application_icon();

// Browser-integration mark (colored, ui-assets/app-icon/extension-icon.svg),
// used by the About window and matching the extension's toolbar icon.
QIcon extension_icon();

// Tray mark: the colored browser-integration mark or the monochrome variant
// (ui-assets/app-icon/single-color-icon.svg) tinted for the current theme.
QIcon tray_icon(TrayIconStyle style = TrayIconStyle::Colorful);

// Filled glyphs for the icon area of the message boxes. They replace the
// platform's built-in message icons, so every window of the app follows the
// same look. The pixmaps are rendered at the size a message box uses for its
// icon (32x32).
QPixmap dialog_info_pixmap();
QPixmap dialog_warning_pixmap();
QPixmap dialog_question_pixmap();
QPixmap dialog_complete_pixmap();

}  // namespace swiftdm::ui
