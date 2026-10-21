// Headless check that the toolbar/app SVG icons load and rasterize with
// visible pixels. Only image rendering is exercised — no windows are shown,
// which keeps this within the CLI/backend-only test policy (AGENTS.md §2).

#include <QApplication>
#include <QIcon>
#include <QImage>
#include <QList>
#include <QPixmap>
#include <QSet>
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

// The icons are stored premultiplied, so the color of a pixel has to be
// unpremultiplied before the hues can be compared.
QRgb unpremultiply(QRgb pixel) {
    const int alpha = qAlpha(pixel);
    if (alpha == 0) return 0;
    const int r = qMin(255, qRed(pixel) * 255 / alpha);
    const int g = qMin(255, qGreen(pixel) * 255 / alpha);
    const int b = qMin(255, qBlue(pixel) * 255 / alpha);
    return qRgba(r, g, b, alpha);
}

// Number of distinct hues among the opaque pixels; a colored mark has more
// than one, a monochrome one has exactly one. The antialiased edge pixels are
// skipped and nearby colors are merged, because the rounding of the alpha
// blending shifts their values a little.
int distinct_colors(const QImage& image, int tolerance = 12) {
    QList<QRgb> colors;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QRgb pixel = image.pixel(x, y);
            if (qAlpha(pixel) < 128) continue;
            const QRgb color = unpremultiply(pixel) | 0xff000000;
            bool known = false;
            for (const QRgb other : colors) {
                if (qAbs(qRed(color) - qRed(other)) <= tolerance &&
                    qAbs(qGreen(color) - qGreen(other)) <= tolerance &&
                    qAbs(qBlue(color) - qBlue(other)) <= tolerance) {
                    known = true;
                    break;
                }
            }
            if (!known) colors << color;
        }
    }
    return static_cast<int>(colors.size());
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
        QStringLiteral(":/icons/info-filled.svg"),  QStringLiteral(":/icons/allert-filled.svg"),
        QStringLiteral(":/icons/question-filled.svg"), QStringLiteral(":/icons/complete-filled.svg"),
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

    // The message-box glyphs are tinted copies of the filled SVG icons: they
    // must render as a solid, single-colored mark on a 32x32 canvas.
    const QList<QPixmap> dialog_pixmaps = {
        swiftdm::ui::dialog_info_pixmap(),
        swiftdm::ui::dialog_warning_pixmap(),
        swiftdm::ui::dialog_question_pixmap(),
        swiftdm::ui::dialog_complete_pixmap(),
    };
    for (const QPixmap& pixmap : dialog_pixmaps) {
        if (pixmap.isNull() || pixmap.size() != QSize(32, 32)) {
            std::printf("FAIL dialog icon: missing or not 32x32\n");
            ++failures;
            continue;
        }
        const QImage image = pixmap.toImage();
        const int opaque = opaque_pixels(image);
        if (opaque == 0) {
            std::printf("FAIL dialog icon: rendered empty\n");
            ++failures;
        } else if (distinct_colors(image) != 1) {
            std::printf("FAIL dialog icon is not monochrome (%d colors)\n",
                        distinct_colors(image));
            ++failures;
        } else {
            std::printf("ok   dialog icon (%d px)\n", opaque);
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
    // the tray shows the same colored mark.
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
    } else if (distinct_colors(tray) < 2) {
        std::printf("FAIL tray icon is not the colored mark\n");
        ++failures;
    } else {
        std::printf("ok   tray icon (%d px, %d colors)\n",
                    opaque_pixels(tray), distinct_colors(tray));
    }

    // The "simple" tray icon is the monochrome mark: one color only.
    const QImage simple_tray = swiftdm::ui::tray_icon(
        swiftdm::ui::TrayIconStyle::Simple).pixmap(22, 22).toImage();
    if (simple_tray.isNull() || opaque_pixels(simple_tray) == 0) {
        std::printf("FAIL simple tray icon missing or empty\n");
        ++failures;
    } else if (distinct_colors(simple_tray) != 1) {
        std::printf("FAIL simple tray icon is not monochrome (%d colors)\n",
                    distinct_colors(simple_tray));
        ++failures;
    } else {
        std::printf("ok   simple tray icon (%d px, monochrome)\n",
                    opaque_pixels(simple_tray));
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
