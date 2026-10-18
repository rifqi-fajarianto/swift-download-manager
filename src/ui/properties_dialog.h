#pragma once

#include <QDialog>

#include "ui/download_model.h"

namespace swiftdm::ui {

class DownloadTask;

// Read-only "Download Properties" window (fields per docs/spec/01-product.md
// §3.6). Opened from the download item's context menu.
class PropertiesDialog : public QDialog {
    Q_OBJECT
  public:
    PropertiesDialog(const DownloadItem& item, const DownloadTask* task,
                     QWidget* parent = nullptr);
};

}  // namespace swiftdm::ui
