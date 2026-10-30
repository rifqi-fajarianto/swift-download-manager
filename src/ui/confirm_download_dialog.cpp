#include "ui/confirm_download_dialog.h"

#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFontMetrics>
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
#include "ui/section_box.h"

namespace swiftdm::ui {
namespace {

// Space taken by the label column, the section margins and the window margins:
// the file name label gets what is left of the window width.
constexpr int kFileLabelChrome = 140;
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

QString default_download_location(const QString& download_dir,
                                  const std::map<std::string, std::string>& category_dirs,
                                  const QString& category, bool category_folders) {
    const QString trimmed_category = category.trimmed();
    const auto custom = category_dirs.find(trimmed_category.toStdString());
    QString base = custom != category_dirs.end()
                       ? QString::fromStdString(custom->second)
                       : download_dir.trimmed();
    if (base.isEmpty()) base = QDir::homePath();
    base = QString::fromStdString(swiftdm::expand_tilde(base.toStdString()));
    if (custom != category_dirs.end()) return QDir::cleanPath(base);
    if (category_folders && !trimmed_category.isEmpty()) {
        return QDir::cleanPath(QDir(base).filePath(trimmed_category));
    }
    return QDir::cleanPath(base);
}

ConfirmDownloadDialog::ConfirmDownloadDialog(
    const QString& file_name, quint64 size, const QString& url, const QString& download_dir,
    const std::map<std::string, std::string>& category_dirs, bool category_folders,
    QWidget* parent)
    : QDialog(parent),
      download_dir_(download_dir),
      category_dirs_(category_dirs),
      category_folders_(category_folders),
      file_name_(file_name.isEmpty() ? tr("(from URL)") : file_name) {
    setWindowTitle(tr("Download Confirmation"));
    setMinimumSize(kMinWindowWidth, kMinWindowHeight);
    resize(kDefaultWindowWidth, kDefaultWindowHeight);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(kSectionSpacing);

    auto* file_section = add_form_section(layout, this, tr("File"));
    file_section->parentWidget()->setObjectName("confirm_file_section");

    // Name: the server-resolved file name. It is elided in the middle so a
    // long name cannot widen the window; the full name stays in the tooltip.
    file_label_ = new QLabel(this);
    file_label_->setObjectName("confirm_file_name");
    file_label_->setText(file_name_);
    file_label_->setToolTip(file_name_);
    file_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    // The label ignores its own size hint horizontally: the form decides how
    // much room it gets, the elided text fits into it.
    file_label_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    file_label_->setMinimumWidth(0);
    file_label_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    file_section->addRow(tr("Name:"), file_label_);

    // Size: the content length reported by the server, on its own row.
    const QString size_text =
        size > 0 ? QString::fromStdString(swiftdm::format_bytes(size, 3))
                 : tr("Unknown size");
    size_label_ = new QLabel(size_text, this);
    size_label_->setObjectName("confirm_file_size");
    size_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    size_label_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    file_section->addRow(tr("Size:"), size_label_);

    auto* save_section = add_form_section(layout, this, tr("Save"));
    save_section->parentWidget()->setObjectName("confirm_save_section");

    // URL: full value in a read-only field (selectable and scrollable).
    auto* url_edit = new QLineEdit(url, this);
    url_edit->setObjectName("confirm_url");
    url_edit->setReadOnly(true);
    url_edit->setCursorPosition(0);
    url_edit->setToolTip(url);
    save_section->addRow(tr("URL:"), url_edit);

    // Category: extension-based default, overridable by the user.
    category_box_ = new QComboBox(this);
    category_box_->setObjectName("confirm_category");
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
    // Keep the combo at its natural width instead of stretching it across the
    // whole section like the fields.
    category_box_->setMaximumWidth(180);
    auto* category_row = new QWidget(this);
    auto* category_layout = new QHBoxLayout(category_row);
    category_layout->setContentsMargins(0, 0, 0, 0);
    category_layout->setSpacing(0);
    category_layout->addWidget(category_box_);
    category_layout->addStretch(1);
    save_section->addRow(tr("Category:"), category_row);

    // Location: category folder by default, overridable with a custom folder.
    auto* location_row = new QHBoxLayout;
    location_row->setContentsMargins(0, 0, 0, 0);
    location_row->setSpacing(6);
    location_edit_ = new QLineEdit(this);
    location_edit_->setObjectName("confirm_location");
    location_edit_->setClearButtonEnabled(true);
    location_edit_->setToolTip(tr("Folder the file is saved to"));
    location_row->addWidget(location_edit_, 1);
    auto* browse_button = new QPushButton(tr("Browse..."), this);
    browse_button->setObjectName("confirm_browse");
    location_row->addWidget(browse_button);
    auto* location_container = new QWidget(this);
    location_container->setLayout(location_row);
    save_section->addRow(tr("Location:"), location_container);

    layout->addStretch(1);

    auto* buttons = new QHBoxLayout;
    buttons->setSpacing(8);
    auto* cancel_button = new QPushButton(tr("Cancel"), this);
    cancel_button->setObjectName("confirm_cancel");
    queue_button_ = new QPushButton(tr("Add to queue"), this);
    queue_button_->setObjectName("confirm_queue");
    queue_button_->setEnabled(false);
    download_button_ = new QPushButton(tr("Download Now"), this);
    download_button_->setObjectName("confirm_download");
    download_button_->setDefault(true);
    buttons->addWidget(cancel_button);
    buttons->addStretch(1);
    buttons->addWidget(queue_button_);
    buttons->addWidget(download_button_);
    layout->addLayout(buttons);

    // Every input and button gets the same height.
    const QList<QWidget*> controls = {url_edit, category_box_, location_edit_,
                                      browse_button, download_button_, queue_button_,
                                      cancel_button};
    const int control_height = common_control_height(controls);
    for (QWidget* widget : controls) widget->setFixedHeight(control_height);

    // Fill the default location only after the category box exists.
    use_default_location();

    connect(category_box_, &QComboBox::currentTextChanged, this, [this] {
        if (!location_custom_) use_default_location();
    });
    connect(category_box_, &QComboBox::activated, this,
            [this](int) { category_custom_ = true; });
    connect(location_edit_, &QLineEdit::textEdited, this, [this] {
        location_custom_ = true;
    });
    connect(location_edit_, &QLineEdit::textChanged, this, [this](const QString& text) {
        const bool valid = probe_ready_ && !text.trimmed().isEmpty();
        download_button_->setEnabled(valid);
        queue_button_->setEnabled(valid && !queue_choices_.isEmpty());
    });
    connect(browse_button, &QPushButton::clicked, this,
            &ConfirmDownloadDialog::choose_location);
    connect(download_button_, &QPushButton::clicked, this, [this] {
        queue_requested_ = false;
        queue_target_ = 0;
        confirm_download();
    });
    connect(queue_button_, &QPushButton::clicked, this,
            &ConfirmDownloadDialog::confirm_queue);
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

void ConfirmDownloadDialog::set_queue_choices(const QVector<QueueChoice>& queues,
                                              qint64 target_id) {
    queue_choices_ = queues;
    queue_target_ = 0;
    if (target_id > 0) {
        for (const QueueChoice& queue : queue_choices_) {
            if (queue.id == target_id) {
                queue_target_ = target_id;
                break;
            }
        }
    }

    const bool valid = probe_ready_ && !location_edit_->text().trimmed().isEmpty();
    queue_button_->setEnabled(valid && !queue_choices_.isEmpty());
    // A known target (the queue the download came from) keeps Add to queue as
    // the default action; otherwise Download Now stays the default.
    const bool queue_default = queue_target_ > 0;
    queue_button_->setDefault(queue_default);
    download_button_->setDefault(!queue_default);
    if (queue_choices_.isEmpty()) {
        queue_button_->setToolTip(
            tr("No queues yet. Create one from the Queues branch first."));
    } else if (queue_target_ > 0) {
        queue_button_->setToolTip(
            tr("Add this download to \"%1\".").arg(queue_target_name()));
    } else {
        queue_button_->setToolTip(tr("Choose the queue this download is added to."));
    }
}

QString ConfirmDownloadDialog::queue_target_name() const {
    for (const QueueChoice& queue : queue_choices_) {
        if (queue.id == queue_target_) return queue.name;
    }
    return {};
}

void ConfirmDownloadDialog::confirm_queue() {
    if (queue_choices_.isEmpty()) return;

    qint64 target = queue_target_;
    if (target <= 0) {
        if (queue_choices_.size() == 1) {
            // A single queue leaves nothing to choose.
            target = queue_choices_.first().id;
        } else {
            QueuePickerDialog picker(queue_choices_, this);
            if (picker.exec() != QDialog::Accepted) return;
            target = picker.selected_queue_id();
        }
    }
    if (target <= 0) return;

    const qint64 previous = queue_target_;
    queue_target_ = target;
    queue_requested_ = true;
    if (!confirm_download()) {
        // Keep the dialog open and ask again on the next click instead of
        // silently reusing a target the user has not confirmed.
        queue_target_ = previous;
        queue_requested_ = false;
    }
}

void ConfirmDownloadDialog::set_probe_pending() {
    probe_ready_ = false;
    size_label_->setText(tr("Checking…"));
    download_button_->setEnabled(false);
    queue_button_->setEnabled(false);
}

void ConfirmDownloadDialog::set_probe_result(const QString& file_name, quint64 size) {
    probe_ready_ = true;
    file_name_ = file_name.isEmpty() ? tr("(from URL)") : file_name;
    file_label_->setToolTip(file_name_);
    update_file_label();
    size_label_->setText(size > 0 ? QString::fromStdString(swiftdm::format_bytes(size, 3))
                                  : tr("Unknown size"));

    if (!category_custom_) {
        const QString category = QString::fromStdString(
            swiftdm::category_folder_for(file_name.toStdString()));
        const QString selected = swiftdm::is_category_name(category.toStdString())
                                     ? category
                                     : QStringLiteral("Misc");
        category_box_->setCurrentText(selected);
        if (!location_custom_) use_default_location();
    }
    download_button_->setEnabled(!location_edit_->text().trimmed().isEmpty());
    queue_button_->setEnabled(!location_edit_->text().trimmed().isEmpty() &&
                              !queue_choices_.isEmpty());
}

QString ConfirmDownloadDialog::default_location() const {
    return default_download_location(download_dir_, category_dirs_,
                                     category_box_->currentText(), category_folders_);
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

bool ConfirmDownloadDialog::confirm_download() {
    const QString dir = location();
    if (dir.isEmpty()) {
        QMessageBox box(this);
        box.setIconPixmap(dialog_warning_pixmap());
        box.setWindowTitle(tr("Download Confirmation"));
        box.setText(tr("Choose a folder the file is saved to."));
        box.exec();
        return false;
    }
    if (!QDir(dir).exists() && !QDir().mkpath(dir)) {
        QMessageBox box(this);
        box.setIconPixmap(dialog_warning_pixmap());
        box.setWindowTitle(tr("Download Confirmation"));
        box.setText(tr("The folder cannot be created:\n%1").arg(dir));
        box.exec();
        return false;
    }
    accept();
    return true;
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
