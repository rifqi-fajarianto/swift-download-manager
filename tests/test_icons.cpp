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

    if (failures > 0) std::printf("%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
