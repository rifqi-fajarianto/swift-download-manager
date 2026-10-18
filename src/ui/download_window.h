#pragma once

#include <QDialog>
#include <QPair>
#include <QVector>

#include "store/settings.h"

class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QTableWidget;
class QWidget;

namespace swiftdm::ui {

class DownloadModel;
class DownloadTask;

// Progress bar showing one slice per segment (green = received bytes).
class SegmentBar : public QWidget {
    Q_OBJECT
  public:
    explicit SegmentBar(QWidget* parent = nullptr);

    void set_segments(const QVector<QPair<quint64, quint64>>& segments);  // (done, length)
    QSize sizeHint() const override;

  protected:
    void paintEvent(QPaintEvent* event) override;

  private:
    QVector<QPair<quint64, quint64>> segments_;
};

// Per-download window: Download / Options / Connections tabs, per
// docs/spec/01-product.md §3.2.
class DownloadWindow : public QDialog {
    Q_OBJECT
  public:
    DownloadWindow(DownloadModel* model, int item_id, swiftdm::Settings settings,
                   QWidget* parent = nullptr);

  private:
    QWidget* build_download_tab();
    QWidget* build_options_tab();
    QWidget* build_connections_tab();
    void refresh();
    void refresh_connections();

    DownloadModel* model_ = nullptr;
    int item_id_ = 0;
    swiftdm::Settings settings_;

    QLabel* url_label_ = nullptr;
    QLabel* status_label_ = nullptr;
    QLabel* size_label_ = nullptr;
    QLabel* downloaded_label_ = nullptr;
    QLabel* bandwidth_label_ = nullptr;
    QLabel* remaining_label_ = nullptr;
    QLabel* resumable_label_ = nullptr;
    QProgressBar* progress_ = nullptr;
    SegmentBar* segment_bar_ = nullptr;
    QLabel* segments_label_ = nullptr;
    QPushButton* resume_button_ = nullptr;
    QPushButton* cancel_button_ = nullptr;

    QLineEdit* dir_edit_ = nullptr;
    QLineEdit* name_edit_ = nullptr;
    QLineEdit* referer_edit_ = nullptr;
    QSpinBox* connections_spin_ = nullptr;
    QSpinBox* limit_spin_ = nullptr;
    QPushButton* apply_button_ = nullptr;

    QTableWidget* connections_table_ = nullptr;
};

}  // namespace swiftdm::ui
