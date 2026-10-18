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

// App window/tray icon (colored, defined in src/ui/icons/app.svg).
QIcon application_icon();

}  // namespace swiftdm::ui
