#include "ui/speed_limit_editor.h"

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QGridLayout>
#include <QSignalBlocker>
#include <QSpinBox>

#include <algorithm>
#include <iterator>

namespace swiftdm::ui {

namespace {

constexpr int kMaxLimitKbps = 1024 * 1024;  // 1 TB/s ceiling for the spin box
constexpr int kDefaultLimitKbps = 1024;     // 1 MB/s

// Predefined presets (KB/s). 1024-based, so the labels read as whole MB/s.
constexpr int kPresetKbps[] = {500, 1024, 5 * 1024, 10 * 1024, 20 * 1024, 50 * 1024};

}  // namespace

QString format_speed_limit(int kbps) {
    if (kbps <= 0) return {};
    if (kbps % 1024 == 0) {
        return QCoreApplication::translate("SpeedLimitEditor", "%1 MB/s")
            .arg(kbps / 1024);
    }
    return QCoreApplication::translate("SpeedLimitEditor", "%1 KB/s").arg(kbps);
}

SpeedLimitEditor::SpeedLimitEditor(QWidget* parent) : QWidget(parent) {
    // Two rows so the toggle and the preset stay readable in the narrower
    // field column of the per-download options: toggle + preset on top, the
    // numeric value below it.
    auto* layout = new QGridLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setHorizontalSpacing(6);
    layout->setVerticalSpacing(6);

    enabled_ = new QCheckBox(tr("Enable speed limiter"), this);
    enabled_->setToolTip(tr("When off, downloads are unlimited. The value is kept "
                            "and can still be changed for later."));
    layout->addWidget(enabled_, 0, 0);

    preset_ = new QComboBox(this);
    preset_->setToolTip(tr("Predefined speed limit"));
    preset_->addItem(tr("Custom"));
    for (const int kbps : kPresetKbps) {
        preset_->addItem(format_speed_limit(kbps));
    }
    layout->addWidget(preset_, 0, 2, Qt::AlignRight);

    kbps_ = new QSpinBox(this);
    kbps_->setRange(1, kMaxLimitKbps);
    kbps_->setSuffix(tr(" KB/s"));
    kbps_->setValue(kDefaultLimitKbps);
    kbps_->setToolTip(tr("Speed limit in KB/s"));
    layout->addWidget(kbps_, 1, 0, Qt::AlignLeft);
    layout->setColumnStretch(1, 1);

    // Picking a preset writes the value; it never toggles the limiter itself.
    connect(preset_, &QComboBox::activated, this, [this](int index) {
        if (index >= 1) kbps_->setValue(kPresetKbps[index - 1]);
    });
    connect(kbps_, qOverload<int>(&QSpinBox::valueChanged), this,
            [this] {
                sync_preset();
                emit_value_edited();
            });
    connect(enabled_, &QCheckBox::toggled, this, [this] { emit_value_edited(); });
    sync_preset();
}

void SpeedLimitEditor::set_value(bool enabled, int kbps) {
    const QSignalBlocker enabled_blocker(enabled_);
    const QSignalBlocker value_blocker(kbps_);
    const QSignalBlocker preset_blocker(preset_);
    enabled_->setChecked(enabled);
    kbps_->setValue(std::clamp(kbps > 0 ? kbps : kDefaultLimitKbps, 1, kMaxLimitKbps));
    sync_preset();
}

bool SpeedLimitEditor::enabled() const { return enabled_->isChecked(); }

int SpeedLimitEditor::kbps() const { return kbps_->value(); }

void SpeedLimitEditor::emit_value_edited() {
    emit valueEdited(enabled(), kbps());
}

void SpeedLimitEditor::sync_preset() {
    const int value = kbps_->value();
    int index = 0;  // Custom
    for (int i = 0; i < static_cast<int>(std::size(kPresetKbps)); ++i) {
        if (kPresetKbps[i] == value) {
            index = i + 1;
            break;
        }
    }
    const QSignalBlocker blocker(preset_);
    preset_->setCurrentIndex(index);
}

}  // namespace swiftdm::ui
