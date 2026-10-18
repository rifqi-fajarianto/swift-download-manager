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
    reader.setScaledSize(QSize(size, size));
    QImage image = reader.read();
    if (image.isNull()) return image;
    if (image.width() != size || image.height() != size) {
        image = image.scaled(size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    return image;
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

}  // namespace

QIcon tinted_icon(const QString& resource_path, const QColor& color) {
    QIcon icon;
    for (const int size : {16, 20, 24, 32, 48}) {
        const QImage image = read_svg(resource_path, size);
        if (image.isNull()) continue;
        icon.addPixmap(QPixmap::fromImage(tinted(image, color)));
    }
    return icon;
}

QIcon application_icon() {
    QIcon icon;
    for (const int size : {16, 24, 32, 48, 64, 128}) {
        QImageReader reader(":/icons/app.svg");
        if (!reader.canRead()) break;
        reader.setScaledSize(QSize(size, size));
        const QImage image = reader.read();
        if (!image.isNull()) icon.addPixmap(QPixmap::fromImage(image));
    }
    return icon;
}

}  // namespace swiftdm::ui
