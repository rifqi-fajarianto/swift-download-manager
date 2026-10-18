#pragma once

#include <QDialog>

class QLineEdit;
class QPushButton;

namespace swiftdm::ui {

// Small "New URL" dialog (URL field + Download/Cancel), per the reference
// layout in docs/spec/01-product.md.
class AddDownloadDialog : public QDialog {
    Q_OBJECT
  public:
    explicit AddDownloadDialog(QWidget* parent = nullptr);

    QString url() const;

  private:
    QLineEdit* url_edit_ = nullptr;
    QPushButton* download_button_ = nullptr;
};

}  // namespace swiftdm::ui
