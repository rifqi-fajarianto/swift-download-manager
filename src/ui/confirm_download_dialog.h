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
//
// The window keeps a fixed geometry: the file name is elided in the middle
// and the URL sits in a read-only field, so neither of them can widen the
// dialog.
class ConfirmDownloadDialog : public QDialog {
    Q_OBJECT
  public:
    // Fixed window geometry (see the class comment).
    static constexpr int kDefaultWindowWidth = 640;
    static constexpr int kDefaultWindowHeight = 250;
    static constexpr int kMinWindowWidth = 520;
    static constexpr int kMinWindowHeight = 230;

    ConfirmDownloadDialog(const QString& file_name, quint64 size, const QString& url,
                          const QString& download_dir, bool category_folders,
                          QWidget* parent = nullptr);

    QString category() const;
    // Exact destination folder, tilde-expanded and cleaned.
    QString location() const;

  protected:
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;

  private:
    void use_default_location();
    void choose_location();
    void confirm_download();
    // Elides the file name so a long name never widens the window.
    void update_file_label();

    QString default_location() const;

    QString download_dir_;
    bool category_folders_ = true;
    // True once the user edited or picked a location: category changes must no
    // longer move it.
    bool location_custom_ = false;
    // Full file name; the label shows an elided form of it.
    QString file_name_;

    QComboBox* category_box_ = nullptr;
    QLineEdit* location_edit_ = nullptr;
    QLabel* file_label_ = nullptr;
    QLabel* size_label_ = nullptr;
    QLabel* queue_hint_ = nullptr;
    QPushButton* download_button_ = nullptr;
};

}  // namespace swiftdm::ui
