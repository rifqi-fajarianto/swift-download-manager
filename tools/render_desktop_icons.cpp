// Regenerates the hicolor icon theme PNGs used by the .desktop entry from the
// SVG sources in ui-assets/app-icon/. Run it after changing the app icon:
//
//   cmake --build build --target render-desktop-icons
//
// Debian policy wants raster icons in the theme, and the desktop shells pick the
// largest matching size for launchers and switchers.

#include <QGuiApplication>
#include <QDir>
#include <QImage>
#include <QImageReader>
#include <QPainter>

#include <cstdio>

namespace {

bool render(const QString& source, const QString& target, int size) {
    QImageReader reader(source);
    if (!reader.canRead()) {
        std::fprintf(stderr, "cannot read %s\n", qPrintable(source));
        return false;
    }
    // The main icon is square (500x500); keep the ratio anyway in case it changes.
    const QSize source_size = reader.size();
    QSize scaled(size, size);
    if (source_size.isValid() && source_size.width() != source_size.height()) {
        scaled = source_size.scaled(size, size, Qt::KeepAspectRatio);
    }
    reader.setScaledSize(scaled);
    const QImage image = reader.read();
    if (image.isNull()) {
        std::fprintf(stderr, "cannot render %s\n", qPrintable(source));
        return false;
    }
    QImage canvas = image;
    if (image.size() != QSize(size, size)) {
        canvas = QImage(size, size, QImage::Format_ARGB32);
        canvas.fill(Qt::transparent);
        QPainter painter(&canvas);
        painter.drawImage((size - image.width()) / 2, (size - image.height()) / 2, image);
        painter.end();
    }
    if (!QDir().mkpath(QFileInfo(target).absolutePath())) {
        std::fprintf(stderr, "cannot create %s\n", qPrintable(QFileInfo(target).absolutePath()));
        return false;
    }
    if (!canvas.save(target, "PNG")) {
        std::fprintf(stderr, "cannot write %s\n", qPrintable(target));
        return false;
    }
    std::printf("wrote %s\n", qPrintable(target));
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);

    const QString root = argc > 1 ? QString::fromUtf8(argv[1])
                                  : QStringLiteral(SWIFTSDM_SOURCE_DIR);
    const QString source = root + QStringLiteral("/ui-assets/app-icon/main-icon.svg");
    const QString base = root + QStringLiteral("/packaging/icons/hicolor");

    bool ok = true;
    for (const int size : {16, 24, 32, 48, 64, 128, 256, 512}) {
        ok = render(source,
                    QStringLiteral("%1/%2x%2/apps/swiftdm.png").arg(base).arg(size),
                    size) && ok;
    }
    return ok ? 0 : 1;
}
