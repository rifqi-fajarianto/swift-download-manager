// Renders the Windows application icon from the app icon SVG.
//
//   render-ico [<source.svg> [<output.ico>]]
//
// Without arguments the source is the app icon in the source tree and the
// output overwrites ui-assets/app-icon/main.ico, which the Windows resource
// script and the Inno Setup installer reference. Windows shows the icon at
// sizes from 16 px (list view) to 256 px (large tiles), so the .ico carries
// every size instead of letting the shell rescale one bitmap.
//
// The frames are plain 32-bit BMP entries rather than PNG-compressed ones:
// Windows Vista and later read both, but Inno Setup keeps its setup icon
// handling compatible with the BMP form.

#include <QGuiApplication>
#include <QImage>
#include <QImageReader>

#include <cstdio>
#include <cstring>
#include <vector>

namespace {

struct Frame {
    int size;
    std::vector<unsigned char> bytes;
};

void put16(std::vector<unsigned char>& out, unsigned value) {
    out.push_back(static_cast<unsigned char>(value & 0xff));
    out.push_back(static_cast<unsigned char>((value >> 8) & 0xff));
}

void put32(std::vector<unsigned char>& out, unsigned value) {
    put16(out, value & 0xffff);
    put16(out, (value >> 16) & 0xffff);
}

std::vector<unsigned char> encode_frame(const QImage& image) {
    const int size = image.width();
    // The AND mask is still part of the format even though the 32-bit pixels
    // carry the alpha channel, so it is written as fully opaque padding.
    const int mask_row = ((size + 31) / 32) * 4;
    const int mask_bytes = mask_row * size;

    std::vector<unsigned char> out;
    out.reserve(40 + size * size * 4 + mask_bytes);
    put32(out, 40);              // BITMAPINFOHEADER
    put32(out, static_cast<unsigned>(size));
    put32(out, static_cast<unsigned>(size * 2));  // XOR pixels plus AND mask
    put16(out, 1);               // planes
    put16(out, 32);              // bits per pixel
    put32(out, 0);               // BI_RGB
    put32(out, 0);               // image size
    put32(out, 0);
    put32(out, 0);
    put32(out, 0);
    put32(out, 0);

    const QImage argb = image.convertToFormat(QImage::Format_ARGB32);
    for (int y = size - 1; y >= 0; --y) {  // BMP rows run bottom-up
        const uchar* row = argb.constScanLine(y);
        out.insert(out.end(), row, row + size * 4);
    }
    out.resize(out.size() + mask_bytes, 0);
    return out;
}

bool render(const QString& source, const QString& target) {
    const int sizes[] = {16, 24, 32, 48, 64, 128, 256};
    std::vector<Frame> frames;
    for (const int size : sizes) {
        QImageReader reader(source);
        if (!reader.canRead()) {
            std::fprintf(stderr, "cannot read %s\n", qPrintable(source));
            return false;
        }
        reader.setScaledSize(QSize(size, size));
        const QImage image = reader.read();
        if (image.isNull()) {
            std::fprintf(stderr, "cannot render %s at %d px\n", qPrintable(source), size);
            return false;
        }
        frames.push_back({size, encode_frame(image)});
    }

    std::vector<unsigned char> out;
    put16(out, 0);  // reserved
    put16(out, 1);  // type: icon
    put16(out, static_cast<unsigned>(frames.size()));
    unsigned offset = 6 + 16 * static_cast<unsigned>(frames.size());
    for (const Frame& frame : frames) {
        out.push_back(static_cast<unsigned char>(frame.size == 256 ? 0 : frame.size));
        out.push_back(static_cast<unsigned char>(frame.size == 256 ? 0 : frame.size));
        out.push_back(0);  // palette entries
        out.push_back(0);  // reserved
        put16(out, 1);     // planes
        put16(out, 32);    // bits per pixel
        put32(out, static_cast<unsigned>(frame.bytes.size()));
        put32(out, offset);
        offset += static_cast<unsigned>(frame.bytes.size());
    }
    for (const Frame& frame : frames) out.insert(out.end(), frame.bytes.begin(), frame.bytes.end());

    std::FILE* file = std::fopen(target.toLocal8Bit().constData(), "wb");
    if (file == nullptr) {
        std::fprintf(stderr, "cannot write %s\n", qPrintable(target));
        return false;
    }
    const bool written = std::fwrite(out.data(), 1, out.size(), file) == out.size();
    std::fclose(file);
    if (!written) {
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

    const QString source = argc > 1
                               ? QString::fromUtf8(argv[1])
                               : QStringLiteral(SWIFTSDM_SOURCE_DIR "/ui-assets/app-icon/main-icon.svg");
    const QString target = argc > 2
                               ? QString::fromUtf8(argv[2])
                               : QStringLiteral(SWIFTSDM_SOURCE_DIR "/ui-assets/app-icon/main.ico");
    return render(source, target) ? 0 : 1;
}
