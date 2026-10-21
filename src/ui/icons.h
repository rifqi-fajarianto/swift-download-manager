#pragma once

#include <QColor>
#include <QIcon>
#include <QString>

namespace swiftdm::ui {

namespace colors {

// Toolbar/action tints matching the reference layout: blue for New/Resume/Stop,
// red for Delete, neutral for the rest.
extern const QColor kActionBlue;
extern const QColor kDanger;
extern const QColor kNeutral;
extern const QColor kOkGreen;

}  // namespace colors

// Loads an SVG from a Qt resource path and re-colors it to a single tint. The
// SVG sources use stroke="currentColor" (rendered black by the image plugin),
// so the alpha shape is used as a monochrome mask.
QIcon tinted_icon(const QString& resource_path, const QColor& color);

// Application/window icon (colored, ui-assets/app-icon/main-icon.svg).
QIcon application_icon();

// Browser-integration mark (colored, ui-assets/app-icon/extension-icon.svg),
// used by the About window and matching the extension's toolbar icon.
QIcon extension_icon();

// Tray mark (colored, ui-assets/app-icon/extension-icon.svg), the same mark
// the browser integration uses.
QIcon tray_icon();

}  // namespace swiftdm::ui
