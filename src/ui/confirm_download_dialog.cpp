#include "ui/confirm_download_dialog.h"

#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFontMetrics>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QList>
#include <QMessageBox>
#include <QPushButton>
#include <QResizeEvent>
#include <QShowEvent>
#include <QTimer>
#include <QVBoxLayout>

#include "core/util.h"
#include "engine/categories.h"
#include "ui/icons.h"

namespace swiftdm::ui {
namespace {

// Space taken by the label column, the grid spacing and the window margins:
// the file name label gets what is left of the window width.
constexpr int kFileLabelChrome = 120;
// The file name is never elided below this width, so it stays readable.
constexpr int kMinFileLabelWidth = 160;

// The style gives buttons more padding than line edits and combos, so the
// controls would end up with different heights and the rows would look
// ragged. One common height keeps the dialog even. The height comes from the
// natural size hints, so no text is clipped on a large font.
int common_control_height(const QList<QWidget*>& controls) {
    int height = 0;
    for (const QWidget* widget : controls) {
        height = qMax(height, widget->sizeHint().height());
    }
    return height;
}

}  // namespace

ConfirmDownloadDialog::ConfirmDownloadDialog(const QString& file_name, quint64 size,
                                             const QString& url,
                                             const QString& download_dir,
                                             bool category_folders, QWidget* parent)
    : QDialog(parent),
      download_dir_(download_dir),
      category_folders_(category_folders),
      file_name_(file_name.isEmpty() ? tr("(from URL)") : file_name) {
    setWindowTitle(tr("Download Confirmation"));
    setMinimumSize(kMinWindowWidth, kMinWindowHeight);
    resize(kDefaultWindowWidth, kDefaultWindowHeight);

    auto* layout = new QVBoxLayout(this);
    layout->setSpacing(12);

    auto* grid = new QGridLayout;
    grid->setHorizontalSpacing(16);
    grid->setVerticalSpacing(8);

    const auto add_label = [this, grid](int row, const QString& text) {
        // The description is centered in its row: the value widgets next to it
        // are taller and draw their text in the middle, so both must share
        // the same vertical center.
        grid->addWidget(new QLabel(text, this), row, 0,
                        Qt::AlignLeft | Qt::AlignVCenter);
    };

    // File: the server-resolved name. It is elided in the middle so a long
    // name cannot widen the window; the full name stays in the tooltip.
    add_label(0, tr("File :"));
    file_label_ = new QLabel(this);
    file_label_->setText(file_name_);
    file_label_->setToolTip(file_name_);
    file_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    // The label ignores its own size hint horizontally: the layout decides
    // how much room it gets, the elided text fits into it.
    file_label_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    file_label_->setMinimumWidth(0);
    file_label_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    // No alignment: the label fills the grid cell, so its width always
    // matches the space the layout gave it.
    grid->addWidget(file_label_, 0, 1);

    // Size: the content length reported by the server, on its own row.
    add_label(1, tr("Size :"));
    const QString size_text =
        size > 0 ? QString::fromStdString(swiftdm::format_bytes(size, 3))
                 : tr("Unknown size");
    size_label_ = new QLabel(size_text, this);
    size_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    size_label_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    grid->addWidget(size_label_, 1, 1);

    // URL: full value in a read-only field (selectable and scrollable).
    add_label(2, tr("URL :"));
    auto* url_edit = new QLineEdit(url, this);
    url_edit->setReadOnly(true);
    url_edit->setCursorPosition(0);
    url_edit->setToolTip(url);
    grid->addWidget(url_edit, 2, 1);

    // Category: extension-based default, overridable by the user.
    add_label(3, tr("Category :"));
    category_box_ = new QComboBox(this);
    for (const std::string& name : swiftdm::category_names()) {
        category_box_->addItem(QString::fromStdString(name));
    }
    const QString default_category =
        QString::fromStdString(swiftdm::category_folder_for(file_name.toStdString()));
    const int default_index = category_box_->findText(
        swiftdm::is_category_name(default_category.toStdString()) ? default_category
                                                                  : QStringLiteral("Misc"));
    if (default_index >= 0) category_box_->setCurrentIndex(default_index);
    category_box_->setToolTip(
        tr("Used for the category tree and for the default save location."));
    grid->addWidget(category_box_, 3, 1, Qt::AlignLeft);

    // Location: category folder by default, overridable with a custom folder.
    add_label(4, tr("Location :"));
    auto* location_row = new QHBoxLayout;
    location_edit_ = new QLineEdit(this);
    location_edit_->setClearButtonEnabled(true);
    location_edit_->setToolTip(tr("Folder the file is saved to"));
    location_row->addWidget(location_edit_, 1);
    auto* browse_button = new QPushButton(tr("Browse..."), this);
    location_row->addWidget(browse_button);
    auto* location_container = new QWidget(this);
    location_container->setLayout(location_row);
    grid->addWidget(location_container, 4, 1);
    grid->setColumnStretch(1, 1);
    layout->addLayout(grid);

    queue_hint_ = new QLabel(this);
    queue_hint_->setWordWrap(true);
    queue_hint_->setStyleSheet(QStringLiteral("color:#e0a85a;"));
    // Like the file name, the hint must not stretch the window.
    queue_hint_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    queue_hint_->setVisible(false);
    layout->addWidget(queue_hint_);
    layout->addStretch(1);
    layout->addSpacing(4);

    auto* buttons = new QHBoxLayout;
    buttons->addStretch(1);
    download_button_ = new QPushButton(tr("Download Now"), this);
    download_button_->setDefault(true);
    auto* queue_button = new QPushButton(tr("Add to queue"), this);
    queue_button->setToolTip(tr("Not available yet: queueing is not implemented."));
    auto* cancel_button = new QPushButton(tr("Cancel"), this);
    buttons->addWidget(download_button_);
    buttons->addSpacing(16);
    buttons->addWidget(queue_button);
    buttons->addSpacing(16);
    buttons->addWidget(cancel_button);
    buttons->addStretch(1);
    layout->addLayout(buttons);

    // Every input and button gets the same height.
    const QList<QWidget*> controls = {url_edit, category_box_, location_edit_,
                                      browse_button, download_button_, queue_button,
                                      cancel_button};
    const int control_height = common_control_height(controls);
    for (QWidget* widget : controls) widget->setFixedHeight(control_height);

    // Fill the default location only after the category box exists.
    use_default_location();

    connect(category_box_, &QComboBox::currentTextChanged, this, [this] {
        if (!location_custom_) use_default_location();
    });
    connect(location_edit_, &QLineEdit::textEdited, this, [this] {
        location_custom_ = true;
    });
    connect(location_edit_, &QLineEdit::textChanged, this, [this](const QString& text) {
        download_button_->setEnabled(!text.trimmed().isEmpty());
    });
    connect(browse_button, &QPushButton::clicked, this,
            &ConfirmDownloadDialog::choose_location);
    connect(download_button_, &QPushButton::clicked, this,
            &ConfirmDownloadDialog::confirm_download);
    connect(queue_button, &QPushButton::clicked, this, [this] {
        queue_hint_->setText(
            tr("Add to queue is not available yet: queueing is still to be implemented. "
               "Use Download Now or Cancel."));
        queue_hint_->setVisible(true);
    });
    connect(cancel_button, &QPushButton::clicked, this, &QDialog::reject);

    // Elide the file name right away: the window size is already known here.
    update_file_label();
}

QString ConfirmDownloadDialog::category() const {
    return category_box_->currentText();
}

QString ConfirmDownloadDialog::location() const {
    QString text = location_edit_->text().trimmed();
    if (text.isEmpty()) return {};
    text = QString::fromStdString(swiftdm::expand_tilde(text.toStdString()));
    return QDir::cleanPath(text);
}

QString ConfirmDownloadDialog::default_location() const {
    QString base = download_dir_.trimmed();
    if (base.isEmpty()) base = QDir::homePath();
    base = QString::fromStdString(swiftdm::expand_tilde(base.toStdString()));
    if (category_folders_) {
        return QDir::cleanPath(QDir(base).filePath(category_box_->currentText()));
    }
    return QDir::cleanPath(base);
}

void ConfirmDownloadDialog::use_default_location() {
    location_edit_->setText(default_location());
}

void ConfirmDownloadDialog::choose_location() {
    const QString start = location_edit_->text().trimmed().isEmpty()
                              ? default_location()
                              : location();
    const QString dir = QFileDialog::getExistingDirectory(this, tr("Select Download Folder"),
                                                          start);
    if (dir.isEmpty()) return;
    location_custom_ = true;
    location_edit_->setText(QDir::cleanPath(dir));
}

void ConfirmDownloadDialog::confirm_download() {
    const QString dir = location();
    if (dir.isEmpty()) {
        QMessageBox box(this);
        box.setIconPixmap(dialog_warning_pixmap());
        box.setWindowTitle(tr("Download Confirmation"));
        box.setText(tr("Choose a folder the file is saved to."));
        box.exec();
        return;
    }
    if (!QDir(dir).exists() && !QDir().mkpath(dir)) {
        QMessageBox box(this);
        box.setIconPixmap(dialog_warning_pixmap());
        box.setWindowTitle(tr("Download Confirmation"));
        box.setText(tr("The folder cannot be created:\n%1").arg(dir));
        box.exec();
        return;
    }
    accept();
}

void ConfirmDownloadDialog::update_file_label() {
    if (!file_label_) return;
    // The label width is known once the layout ran; before that the window
    // width is the only hint.
    int available = file_label_->width();
    if (available <= 0) available = width() - kFileLabelChrome;
    available = qMax(available, kMinFileLabelWidth);
    const QFontMetrics metrics(file_label_->font());
    file_label_->setText(metrics.elidedText(file_name_, Qt::ElideMiddle, available));
}

void ConfirmDownloadDialog::resizeEvent(QResizeEvent* event) {
    QDialog::resizeEvent(event);
    update_file_label();
    // The layout assigns the label width after the resize event, so refresh
    // once the new geometry is in place.
    QTimer::singleShot(0, this, &ConfirmDownloadDialog::update_file_label);
}

void ConfirmDownloadDialog::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    update_file_label();
    QTimer::singleShot(0, this, &ConfirmDownloadDialog::update_file_label);
}

}  // namespace swiftdm::ui
