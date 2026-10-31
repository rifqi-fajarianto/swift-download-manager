#pragma once

#include <QDialog>
#include <QString>

class QCloseEvent;
class QLabel;
class QPushButton;

namespace swiftdm::ui {

// Small always-on-top window shown while a download waits for a refreshed URL
// from the browser. The browser usually has the focus while the user starts the
// file again, so the window is kept above other windows and offers a single
// action: cancel the wait. Closing it (the title-bar button) cancels as well,
// except when MainWindow dismisses it itself after a successful refresh.
//
// The layout is deliberately compact: one icon + headline row, a two-row form
// (file name, file size) and a single button.
class RefreshWaitingDialog : public QDialog {
    Q_OBJECT
  public:
    explicit RefreshWaitingDialog(QWidget* parent = nullptr);

    // Download being waited for; shown as the File Name / File Size rows.
    void set_file_info(const QString& name, quint64 size);

    // Closes the window without emitting cancelRequested(). Used by MainWindow
    // when the wait ends on its own (URL applied, row deleted).
    void dismiss();

  signals:
    void cancelRequested();

  protected:
    void closeEvent(QCloseEvent* event) override;

  private:
    void update_file_name();

    QLabel* name_value_ = nullptr;
    QLabel* size_value_ = nullptr;
    QPushButton* cancel_button_ = nullptr;
    QString file_name_;
    bool dismissing_ = false;
};

}  // namespace swiftdm::ui
