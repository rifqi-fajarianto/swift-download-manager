#pragma once

#include <QWidget>

class QCheckBox;
class QComboBox;
class QSpinBox;

namespace swiftdm::ui {

// Shared speed-limiter editor: an enable toggle, a predefined preset selector
// and the value in KB/s. The value stays editable (and is kept) while the
// toggle is off, so a limit can be prepared before it is enabled.
class SpeedLimitEditor : public QWidget {
    Q_OBJECT
  public:
    explicit SpeedLimitEditor(QWidget* parent = nullptr);

    // Loads the toggle and the value. A missing/zero value falls back to the
    // default preset (1 MB/s) so enabling the limiter never means "0 KB/s".
    void set_value(bool enabled, int kbps);

    bool enabled() const;
    int kbps() const;

  signals:
    void valueEdited(bool enabled, int kbps);

  private:
    void sync_preset();
    void emit_value_edited();

    QCheckBox* enabled_ = nullptr;
    QComboBox* preset_ = nullptr;
    QSpinBox* kbps_ = nullptr;
};

// "500 KB/s" / "5 MB/s" for labels; empty for a non-positive value.
QString format_speed_limit(int kbps);

}  // namespace swiftdm::ui
