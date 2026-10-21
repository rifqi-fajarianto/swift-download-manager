#include "ui/icons.h"

#include <QImage>
#include <QImageReader>
#include <QPainter>
#include <QPixmap>

namespace swiftdm::ui {

namespace colors {
const QColor kActionBlue(0x5a, 0xa9, 0xe6);
const QColor kDanger(0xe0, 0x5a, 0x5a);
const QColor kNeutral(0xd6, 0xd6, 0xd6);
const QColor kOkGreen(0x7e, 0xc8, 0x7e);
}  // namespace colors

namespace {

QImage read_svg(const QString& path, int size) {
    QImageReader reader(path);
    if (!reader.canRead()) return QImage();
    // SVG sources are not always square. Scale the longer side to `size` and
    // center the result on a transparent square canvas so the mark keeps its
    // proportions.
    const QSize source = reader.size();
    QSize target(size, size);
    if (source.isValid() && source.width() != source.height()) {
        target = source.scaled(size, size, Qt::KeepAspectRatio);
    }
    reader.setScaledSize(target);
    const QImage scaled = reader.read();
    if (scaled.isNull()) return scaled;
    if (scaled.size() == QSize(size, size)) return scaled;
    QImage canvas(size, size, QImage::Format_ARGB32_Premultiplied);
    canvas.fill(Qt::transparent);
    QPainter painter(&canvas);
    painter.drawImage((size - scaled.width()) / 2, (size - scaled.height()) / 2, scaled);
    painter.end();
    return canvas;
}

QImage tinted(const QImage& source, const QColor& color) {
    QImage out(source.size(), QImage::Format_ARGB32_Premultiplied);
    out.fill(Qt::transparent);
    QPainter painter(&out);
    painter.drawImage(0, 0, source);
    painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
    painter.fillRect(out.rect(), color);
    painter.end();
    return out;
}

// Rasterizes a colored SVG at the requested sizes. The image plugin keeps the
// source gradients, so no tint is applied here.
QIcon rasterized(const QString& resource_path, std::initializer_list<int> sizes) {
    QIcon icon;
    for (const int size : sizes) {
        const QImage image = read_svg(resource_path, size);
        if (!image.isNull()) icon.addPixmap(QPixmap::fromImage(image));
    }
    return icon;
}

}  // namespace

QIcon tinted_icon(const QString& resource_path, const QColor& color) {
    QIcon icon;
    for (const int size : {16, 20, 22, 24, 32, 48}) {
        const QImage image = read_svg(resource_path, size);
        if (image.isNull()) continue;
        icon.addPixmap(QPixmap::fromImage(tinted(image, color)));
    }
    return icon;
}

QIcon application_icon() {
    return rasterized(QStringLiteral(":/icons/app-icon/main-icon.svg"),
                      {16, 24, 32, 48, 64, 128, 256});
}

QIcon extension_icon() {
    return rasterized(QStringLiteral(":/icons/app-icon/extension-icon.svg"),
                      {16, 24, 32, 48, 64, 128});
}

QIcon tray_icon() {
    // The tray shows the same colored mark as the browser integration, so the
    // app is recognizable in the system tray.
    return rasterized(QStringLiteral(":/icons/app-icon/extension-icon.svg"),
                      {16, 20, 22, 24, 32, 48});
}

}  // namespace swiftdm::ui
