#pragma once

#include <QDialog>

namespace swiftdm::ui {

// Confirmation step for manual downloads: shows the resolved file name and
// size (from the probe) before the transfer starts.
class ConfirmDownloadDialog : public QDialog {
    Q_OBJECT
  public:
    ConfirmDownloadDialog(const QString& file_name, quint64 size, const QString& url,
                          QWidget* parent = nullptr);
};

}  // namespace swiftdm::ui
