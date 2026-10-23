#pragma once

#include <QDialog>

#include "store/history.h"

class QCheckBox;
class QLineEdit;
class QSpinBox;

namespace swiftdm::ui {

class SpeedLimitEditor;

// Per-queue settings window. Queues are independent workspaces, so their
// limits and notifications live here instead of in the global Settings tabs.
class QueuePropertiesDialog : public QDialog {
    Q_OBJECT
  public:
    static constexpr int kDefaultWindowWidth = 520;
    static constexpr int kDefaultWindowHeight = 600;

    QueuePropertiesDialog(const swiftdm::HistoryStore::QueueRecord& queue,
                          int item_count, QWidget* parent = nullptr);

    // Updated copy of the record (the id is preserved).
    swiftdm::HistoryStore::QueueRecord result() const;

  private:
    swiftdm::HistoryStore::QueueRecord queue_;

    QLineEdit* name_ = nullptr;
    QSpinBox* max_simultaneous_ = nullptr;
    QSpinBox* connections_ = nullptr;
    SpeedLimitEditor* limit_ = nullptr;
    QCheckBox* autostart_ = nullptr;
    QCheckBox* notify_item_ = nullptr;
    QCheckBox* notify_complete_ = nullptr;
    QCheckBox* shutdown_ = nullptr;
    QSpinBox* shutdown_delay_ = nullptr;
};

}  // namespace swiftdm::ui
