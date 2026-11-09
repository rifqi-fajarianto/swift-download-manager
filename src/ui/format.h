#pragma once

#include <QString>

#include <cstdint>

#include "core/util.h"

namespace swiftdm::ui {

// QString wrappers around the core formatters. The UI shows the same decimal
// units as the engine and the original application ("100.000 MB",
// "27.653 MB/sec"), so these stay thin: they only convert the std::string the
// core returns. Header-only because the offscreen tests compile the dialog
// translation units on their own (see tests/CMakeLists.txt), so a new .cpp
// would have to be added to every one of those targets.

inline QString format_bytes_q(quint64 bytes) {
    return QString::fromStdString(swiftdm::format_bytes(bytes, 3));
}

inline QString format_speed_q(double bytes_per_sec) {
    return QString::fromStdString(swiftdm::format_speed(bytes_per_sec, 3));
}

inline QString format_duration_q(qint64 seconds) {
    return QString::fromStdString(swiftdm::format_duration(seconds));
}

}  // namespace swiftdm::ui
