// Rasterizes the extension's toolbar icons from the SVG sources in
// ui-assets/app-icon/. Chromium cannot load SVG for the action icon, so the
// PNGs are checked in; this tool keeps them reproducible after an icon change.
//
// Usage: cmake --build build --target render-extension-icons
//        ./build/render-extension-icons            (repo root auto-detected)
//        ./build/render-extension-icons <repo-root>

#include <QGuiApplication>
#include <QDir>
#include <QImage>
#include <QImageReader>
#include <QPainter>
#include <QStringList>

#include <cstdio>

namespace {

bool render(const QString& source, const QString& target_dir, const QString& prefix) {
    bool ok = true;
    for (const int size : {16, 32, 48, 128}) {
        QImageReader reader(source);
        if (!reader.canRead()) {
            std::fprintf(stderr, "cannot read %s\n", qPrintable(source));
            return false;
        }
        // Keep the mark's proportions: these SVGs are 351x380, not square.
        const QSize source_size = reader.size();
        QSize target(size, size);
        if (source_size.isValid() && source_size.width() != source_size.height()) {
            target = source_size.scaled(size, size, Qt::KeepAspectRatio);
        }
        reader.setScaledSize(target);
        const QImage scaled = reader.read();
        if (scaled.isNull()) {
            std::fprintf(stderr, "cannot render %s\n", qPrintable(source));
            return false;
        }
        QImage image = scaled;
        if (scaled.size() != QSize(size, size)) {
            image = QImage(size, size, QImage::Format_ARGB32);
            image.fill(Qt::transparent);
            QPainter painter(&image);
            painter.drawImage((size - scaled.width()) / 2, (size - scaled.height()) / 2, scaled);
            painter.end();
        }
        const QString path = QStringLiteral("%1/%2%3.png").arg(target_dir, prefix).arg(size);
        if (!image.save(path, "PNG")) {
            std::fprintf(stderr, "cannot write %s\n", qPrintable(path));
            ok = false;
        } else {
            std::printf("wrote %s\n", qPrintable(path));
        }
    }
    return ok;
}

}  // namespace

int main(int argc, char** argv) {
    // No display is needed; rendering happens in memory.
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);

    const QString root = argc > 1 ? QString::fromUtf8(argv[1])
                                  : QStringLiteral(SWIFTSDM_SOURCE_DIR);
    const QString assets = root + QStringLiteral("/ui-assets/app-icon");
    const QString icons = root + QStringLiteral("/ext/chromium/icons");
    if (!QDir().mkpath(icons)) {
        std::fprintf(stderr, "cannot create %s\n", qPrintable(icons));
        return 1;
    }

    const bool active = render(assets + QStringLiteral("/extension-icon.svg"), icons,
                               QStringLiteral("icon"));
    const bool off = render(assets + QStringLiteral("/extension-diactivate-icon.svg"), icons,
                            QStringLiteral("icon-off"));
    return active && off ? 0 : 1;
}
