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
class SpeedLimitEditor;

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

  signals:
    // Resume was requested in this window; the main window restarts the task
    // so the history row is updated too.
    void resumeRequested(int item_id);
    // The Options tab changed the destination/name; the main window updates the
    // item and its history row.
    void optionsApplied(int item_id);

  private:
    QWidget* build_download_tab();
    QWidget* build_options_tab();
    QWidget* build_connections_tab();
    void refresh();
    void refresh_connections();
    // Shows the URL as an elided link so its length can never widen the
    // window; the full URL stays in the href and the tooltip.
    void update_url_label();

  protected:
    void resizeEvent(QResizeEvent* event) override;

    DownloadModel* model_ = nullptr;
    int item_id_ = 0;
    swiftdm::Settings settings_;

    QString url_text_;
    QLabel* url_label_ = nullptr;
    QLabel* status_label_ = nullptr;
    QLabel* size_label_ = nullptr;
    QLabel* downloaded_label_ = nullptr;
    QLabel* bandwidth_label_ = nullptr;
    QLabel* limit_info_label_ = nullptr;  // "Speed limit: 5 MB/s" while active
    QLabel* remaining_label_ = nullptr;
    QLabel* resumable_label_ = nullptr;
    QProgressBar* progress_ = nullptr;
    SegmentBar* segment_bar_ = nullptr;
    QLabel* segments_label_ = nullptr;
    QPushButton* action_button_ = nullptr;  // Pause while running, Resume when stopped
    QPushButton* cancel_button_ = nullptr;

    QLineEdit* dir_edit_ = nullptr;
    QLineEdit* name_edit_ = nullptr;
    QLineEdit* referer_edit_ = nullptr;
    QSpinBox* connections_spin_ = nullptr;
    SpeedLimitEditor* limit_editor_ = nullptr;
    QPushButton* apply_button_ = nullptr;

    QTableWidget* connections_table_ = nullptr;

    // True once the item was seen in a non-complete state: the window then
    // closes itself when the download finishes (a window opened on an
    // already-complete item stays open for inspection).
    bool seen_incomplete_ = false;
};

}  // namespace swiftdm::ui
