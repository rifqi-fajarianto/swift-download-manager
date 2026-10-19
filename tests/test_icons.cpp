// Headless check that the toolbar/app SVG icons load and rasterize with
// visible pixels. Only image rendering is exercised — no windows are shown,
// which keeps this within the CLI/backend-only test policy (AGENTS.md §2).

#include <QApplication>
#include <QIcon>
#include <QImage>
#include <QPixmap>
#include <QStringList>

#include <cstdio>

#include "ui/icons.h"

namespace {

int opaque_pixels(const QImage& image) {
    int count = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (qAlpha(image.pixel(x, y)) > 0) ++count;
        }
    }
    return count;
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    int failures = 0;

    const QStringList tinted_icons = {
        QStringLiteral(":/icons/alert.svg"),        QStringLiteral(":/icons/applications.svg"),
        QStringLiteral(":/icons/complete.svg"),     QStringLiteral(":/icons/compressed.svg"),
        QStringLiteral(":/icons/delete-file.svg"),  QStringLiteral(":/icons/delete.svg"),
        QStringLiteral(":/icons/document.svg"),     QStringLiteral(":/icons/folder.svg"),
        QStringLiteral(":/icons/link-plus.svg"),    QStringLiteral(":/icons/link.svg"),
        QStringLiteral(":/icons/music.svg"),        QStringLiteral(":/icons/other.svg"),
        QStringLiteral(":/icons/plus.svg"),         QStringLiteral(":/icons/properties.svg"),
        QStringLiteral(":/icons/resume-start.svg"), QStringLiteral(":/icons/settings.svg"),
        QStringLiteral(":/icons/stop.svg"),         QStringLiteral(":/icons/video.svg"),
        QStringLiteral(":/icons/browsers.svg"),     QStringLiteral(":/icons/about.svg"),
        QStringLiteral(":/icons/quit.svg"),
    };
    for (const QString& resource : tinted_icons) {
        const QIcon icon = swiftdm::ui::tinted_icon(resource, Qt::white);
        if (icon.isNull()) {
            std::printf("FAIL %s: icon is null\n", qPrintable(resource));
            ++failures;
            continue;
        }
        const QImage image = icon.pixmap(24, 24).toImage();
        const int opaque = opaque_pixels(image);
        if (opaque == 0) {
            std::printf("FAIL %s: rendered empty\n", qPrintable(resource));
            ++failures;
        } else {
            std::printf("ok   %s (%d px)\n", qPrintable(resource), opaque);
        }
    }

    const QIcon app_icon = swiftdm::ui::application_icon();
    if (app_icon.isNull() || opaque_pixels(app_icon.pixmap(48, 48).toImage()) == 0) {
        std::printf("FAIL app icon missing or empty\n");
        ++failures;
    } else {
        std::printf("ok   app icon\n");
    }

    // The colored extension mark keeps its gradients (more than one hue), and
    // the tray mark is tinted to one color while keeping the shape.
    const QImage extension = swiftdm::ui::extension_icon().pixmap(48, 48).toImage();
    if (extension.isNull() || opaque_pixels(extension) == 0) {
        std::printf("FAIL extension icon missing or empty\n");
        ++failures;
    } else {
        std::printf("ok   extension icon (%d px)\n", opaque_pixels(extension));
    }
    const QImage tray = swiftdm::ui::tray_icon().pixmap(22, 22).toImage();
    if (tray.isNull() || opaque_pixels(tray) == 0) {
        std::printf("FAIL tray icon missing or empty\n");
        ++failures;
    } else {
        std::printf("ok   tray icon (%d px)\n", opaque_pixels(tray));
    }

    // The extension PNGs must exist at every size the manifest and the
    // active/off switch reference; Chromium cannot load SVG toolbar icons.
    const QStringList extension_pngs = {
        QStringLiteral("icons/icon16.png"),      QStringLiteral("icons/icon32.png"),
        QStringLiteral("icons/icon48.png"),      QStringLiteral("icons/icon128.png"),
        QStringLiteral("icons/icon-off16.png"),  QStringLiteral("icons/icon-off32.png"),
        QStringLiteral("icons/icon-off48.png"),  QStringLiteral("icons/icon-off128.png"),
    };
    for (const QString& png : extension_pngs) {
        const QString path =
            QStringLiteral("%1/ext/chromium/%2").arg(QStringLiteral(SWIFTSDM_SOURCE_DIR), png);
        const QImage image(path);
        if (image.isNull() || opaque_pixels(image) == 0) {
            std::printf("FAIL %s: missing or empty\n", qPrintable(path));
            ++failures;
        } else {
            std::printf("ok   %s (%dx%d)\n", qPrintable(png), image.width(), image.height());
        }
    }

    if (failures > 0) std::printf("%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
