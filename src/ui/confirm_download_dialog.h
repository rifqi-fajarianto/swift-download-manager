#pragma once

#include <QDialog>
#include <QString>
#include <QVector>

#include <map>
#include <string>

#include "ui/queue_picker_dialog.h"

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;

namespace swiftdm::ui {

// Default destination folder of a download: the download root plus the
// category folder when the category folders are enabled. The dialog and the
// automatic download path share it, so a download that skips the confirmation
// ends up in the same place as a confirmed one.
QString default_download_location(const QString& download_dir,
                                  const std::map<std::string, std::string>& category_dirs,
                                  const QString& category, bool category_folders);

// Confirmation step shown before any transfer starts (manual "New URL" and
// browser handoff): the server-resolved file name and size, the category
// (extension-based default, user overridable), the destination folder
// (category folder by default, user overridable) and the target queue.
// "Download Now" targets the main Downloads list, while "Add to queue" asks
// for the queue (or uses the one the download came from); Cancel leaves no row
// and no database entry behind.
//
// The window keeps a fixed geometry: the file name is elided in the middle
// and the URL sits in a read-only field, so neither of them can widen the
// dialog.
class ConfirmDownloadDialog : public QDialog {
    Q_OBJECT
  public:
    // Fixed window geometry (see the class comment).
    static constexpr int kDefaultWindowWidth = 650;
    static constexpr int kDefaultWindowHeight = 330;
    static constexpr int kMinWindowWidth = 560;
    static constexpr int kMinWindowHeight = 320;

    ConfirmDownloadDialog(const QString& file_name, quint64 size, const QString& url,
                          const QString& download_dir,
                          const std::map<std::string, std::string>& category_dirs,
                          bool category_folders, QWidget* parent = nullptr);

    QString category() const;
    // Exact destination folder, tilde-expanded and cleaned.
    QString location() const;
    // Queues offered by Add to queue. A target id that exists in the list is
    // used directly (no picker) and makes Add to queue the default button,
    // which is how a download started from a queue view keeps its queue.
    void set_queue_choices(const QVector<QueueChoice>& queues, qint64 target_id = 0);
    // Queue chosen with Add to queue; 0 while Download Now is the action.
    qint64 selected_queue_id() const { return queue_target_; }
    // True when the user accepted with Add to queue rather than Download Now.
    bool queue_requested() const { return queue_requested_; }
    void set_probe_pending();
    void set_probe_result(const QString& file_name, quint64 size);

  protected:
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;

  private:
    void use_default_location();
    void choose_location();
    // Returns false when the destination is invalid and the dialog stays open.
    bool confirm_download();
    // Opens the queue picker (or resolves a single queue) and confirms.
    void confirm_queue();
    // Elides the file name so a long name never widens the window.
    void update_file_label();

    QString default_location() const;
    // Name of the queue that Add to queue would use, empty when there is none.
    QString queue_target_name() const;

    QString download_dir_;
    std::map<std::string, std::string> category_dirs_;
    bool category_folders_ = true;
    // True once the user edited or picked a location: category changes must no
    // longer move it.
    bool location_custom_ = false;
    bool category_custom_ = false;
    bool probe_ready_ = true;
    bool queue_requested_ = false;
    // Full file name; the label shows an elided form of it.
    QString file_name_;
    QVector<QueueChoice> queue_choices_;
    // Target queue when it is already known (queue view) or was picked.
    qint64 queue_target_ = 0;

    QComboBox* category_box_ = nullptr;
    QLineEdit* location_edit_ = nullptr;
    QLabel* file_label_ = nullptr;
    QLabel* size_label_ = nullptr;
    QPushButton* download_button_ = nullptr;
    QPushButton* queue_button_ = nullptr;
};

}  // namespace swiftdm::ui
