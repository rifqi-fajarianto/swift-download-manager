#pragma once

#include <QDialog>
#include <QString>
#include <QVector>

class QListWidget;

namespace swiftdm::ui {

// One entry of the queue picker and of the confirmation dialog's queue list.
struct QueueChoice {
    qint64 id = 0;
    QString name;
    int item_count = 0;
};

// Asks which queue a download belongs to. The Download Confirmation window
// opens it when more than one queue exists, so the target is never guessed.
// The list starts without a selection: the user has to pick a queue, which
// keeps the previous "first queue wins" behavior from happening by accident.
class QueuePickerDialog : public QDialog {
    Q_OBJECT
  public:
    static constexpr int kDefaultWindowWidth = 420;
    static constexpr int kDefaultWindowHeight = 360;
    static constexpr int kMinWindowWidth = 360;
    static constexpr int kMinWindowHeight = 280;

    explicit QueuePickerDialog(const QVector<QueueChoice>& queues,
                               QWidget* parent = nullptr);

    // 0 until the user selects a queue and accepts the dialog.
    qint64 selected_queue_id() const;

  private:
    QListWidget* list_ = nullptr;
};

}  // namespace swiftdm::ui
