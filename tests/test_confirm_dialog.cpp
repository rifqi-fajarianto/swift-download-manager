// Headless check of the download confirmation dialog: the window keeps a
// fixed geometry and the file size sits on its own row. The dialog is never
// shown, so this stays within the CLI/backend-only test policy (AGENTS.md §6).

#include <QApplication>
#include <QFontMetrics>
#include <QLabel>
#include <QLayout>
#include <QStringList>

#include <cstdio>

#include "core/util.h"
#include "ui/confirm_download_dialog.h"

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

QStringList label_texts(const QWidget* widget) {
    QStringList texts;
    for (const QLabel* label : widget->findChildren<QLabel*>()) texts << label->text();
    return texts;
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);

    using swiftdm::ui::ConfirmDownloadDialog;

    const QString long_name = QString(400, QLatin1Char('n')) + QStringLiteral(".mp4");
    const QString long_url =
        QStringLiteral("https://example.com/") + QString(400, QLatin1Char('p'));

    ConfirmDownloadDialog dialog(QStringLiteral("movie.mp4"), 1500000,
                                 QStringLiteral("https://example.com/movie.mp4"),
                                 QStringLiteral("/tmp"), true);

    check(dialog.minimumSize() == QSize(ConfirmDownloadDialog::kMinWindowWidth,
                                        ConfirmDownloadDialog::kMinWindowHeight),
          "the dialog declares a fixed minimum size");
    check(dialog.size() == QSize(ConfirmDownloadDialog::kDefaultWindowWidth,
                                 ConfirmDownloadDialog::kDefaultWindowHeight),
          "the dialog opens with the default size");
    check(dialog.layout()->sizeHint().height() <= ConfirmDownloadDialog::kDefaultWindowHeight,
          "the content fits into the default window height");

    // The size has its own row and is no longer inline with the file name.
    // The expected text comes from the shared formatter: the decimal
    // separator follows the locale.
    const QString size_text =
        QString::fromStdString(swiftdm::format_bytes(1500000, 3));
    const QStringList texts = label_texts(&dialog);
    check(texts.contains(QStringLiteral("Size :")), "the dialog has a separate size row");
    check(texts.contains(size_text), "the size row shows the formatted size");
    bool inline_size = false;
    for (const QString& text : texts) {
        if (text.contains(QStringLiteral("(%1").arg(size_text)) ||
            text.contains(QStringLiteral("%1)").arg(size_text))) {
            inline_size = true;
        }
    }
    check(!inline_size, "the size is no longer shown inline with the file name");

    ConfirmDownloadDialog unknown(QStringLiteral("movie.mp4"), 0,
                                  QStringLiteral("https://example.com/movie.mp4"),
                                  QStringLiteral("/tmp"), true);
    check(label_texts(&unknown).contains(QStringLiteral("Unknown size")),
          "an unknown size is shown");

    // A long file name or URL never widens the window.
    ConfirmDownloadDialog long_dialog(long_name, 1500000, long_url, QStringLiteral("/tmp"),
                                      true);
    check(long_dialog.size() == dialog.size(),
          "a long file name does not change the window size");
    check(long_dialog.layout()->minimumSize().width() <= ConfirmDownloadDialog::kMinWindowWidth,
          "a long file name never forces the dialog wider than the minimum");
    check(long_dialog.minimumSizeHint().width() <= ConfirmDownloadDialog::kMinWindowWidth,
          "the dialog minimum size hint stays inside the fixed minimum");

    // The full file name stays available in the tooltip.
    bool tooltip_ok = false;
    for (const QLabel* label : long_dialog.findChildren<QLabel*>()) {
        if (label->toolTip() == long_name) tooltip_ok = true;
    }
    check(tooltip_ok, "the full file name is kept in the tooltip");

    // The shown file name fits into the label the layout assigned.
    const QLabel* file_label = nullptr;
    for (const QLabel* label : dialog.findChildren<QLabel*>()) {
        if (label->toolTip() == QStringLiteral("movie.mp4")) file_label = label;
    }
    check(file_label != nullptr, "the dialog has a file name label");
    if (file_label) {
        dialog.layout()->activate();
        const QFontMetrics metrics(file_label->font());
        check(metrics.horizontalAdvance(file_label->text()) <= file_label->width() + 1,
              "the shown file name fits into the label width");
        check(file_label->width() <= dialog.width(),
              "the file name label stays inside the window");
    }

    if (failures == 0) std::printf("confirm_dialog: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
