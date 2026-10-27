#pragma once

#include <QString>

#include <cstdint>

#include "store/history.h"
#include "store/settings.h"
#include "ui/download_model.h"
#include "ui/download_task.h"

namespace swiftdm::ui {

// Small helpers shared by the MainWindow translation units (main_window*.cpp).
// They used to live in the anonymous namespace of main_window.cpp; they are
// internal to the ui layer and not part of the public MainWindow interface.
QString now_stamp();
QString stamp_from_unix(int64_t seconds);
qint64 now_ms();
const char* history_status(DownloadItem::State state);
swiftdm::HistoryStore::Record history_record_for(const DownloadItem& item,
                                                 const DownloadTask::Options* options);
DownloadItem::State state_from_record(const swiftdm::HistoryStore::Record& record);
QString find_existing_workspace(const swiftdm::Settings& settings,
                                const swiftdm::HistoryStore::Record& record);

}  // namespace swiftdm::ui
