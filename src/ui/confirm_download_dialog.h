#pragma once

#include <QDialog>
#include <QString>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;

namespace swiftdm::ui {

// Confirmation step shown before any transfer starts (manual "New URL" and
// browser handoff): the server-resolved file name and size, the category
// (extension-based default, user overridable) and the destination folder
// (category folder by default, user overridable). "Add to queue" is a visible
// placeholder until queueing exists; Cancel leaves no row and no database
// entry behind.
class ConfirmDownloadDialog : public QDialog {
    Q_OBJECT
  public:
    ConfirmDownloadDialog(const QString& file_name, quint64 size, const QString& url,
                          const QString& download_dir, bool category_folders,
                          QWidget* parent = nullptr);

    QString category() const;
    // Exact destination folder, tilde-expanded and cleaned.
    QString location() const;

  private:
    void use_default_location();
    void choose_location();
    void confirm_download();

    QString default_location() const;

    QString download_dir_;
    bool category_folders_ = true;
    // True once the user edited or picked a location: category changes must no
    // longer move it.
    bool location_custom_ = false;

    QComboBox* category_box_ = nullptr;
    QLineEdit* location_edit_ = nullptr;
    QLabel* queue_hint_ = nullptr;
    QPushButton* download_button_ = nullptr;
};

}  // namespace swiftdm::ui
