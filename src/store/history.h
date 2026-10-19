#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct sqlite3;

namespace swiftdm {

// SQLite-backed download history (schema in docs/spec/02-data-model.md).
//
// Rows are written when a download is added and updated while it runs, so the
// browser-sent page URL/title survive a restart. MainWindow::restore_history()
// loads them back into the table at startup.
class HistoryStore {
  public:
    struct Record {
        std::string url;
        std::string method = "GET";
        std::string filename;
        std::string category;
        std::string status;  // Downloading / Paused / Complete / Error / Canceled
        std::string user_agent;
        std::string page_url;
        std::string page_title;
        std::string group_id;  // adaptive pair token (video + audio handoff)
        std::string mime_type;
        std::string error_text;
        std::string folder_path;
        // True when folder_path already is the exact destination folder (the
        // confirmation dialog computes the category subfolder itself). False
        // for rows written before this column existed: folder_path is then the
        // base folder and the category subfolder is appended on restore.
        bool folder_exact = false;
        std::string temp_path;
        std::string post_data;
        std::vector<std::string> headers;  // "Name: value" lines
        uint64_t file_size = 0;
        uint64_t done = 0;
        int bandwidth_limit_kbps = 0;
        int connections = 0;
        bool resumable = false;
        int64_t first_try = 0;  // unix seconds
        int64_t last_try = 0;
    };

    // One stored row; `id` is the database primary key.
    struct Entry {
        int64_t id = 0;
        Record record;
    };

    HistoryStore() = default;
    ~HistoryStore();
    HistoryStore(const HistoryStore&) = delete;
    HistoryStore& operator=(const HistoryStore&) = delete;

    // Creates the directory and the tables when needed. A failure leaves the
    // store closed; callers keep running without history.
    bool open(const std::string& path, std::string* error = nullptr);
    bool is_open() const { return db_ != nullptr; }
    void close();

    // Inserts a row and returns its id, or 0 on failure.
    int64_t insert(const Record& record, std::string* error = nullptr);
    bool update(int64_t id, const Record& record, std::string* error = nullptr);
    bool update_status(int64_t id, const std::string& status, const std::string& error_text,
                       std::string* error = nullptr);
    // Lightweight progress write used while a transfer runs (keeps lasttry).
    bool update_progress(int64_t id, uint64_t done, uint64_t file_size,
                         std::string* error = nullptr);
    bool fetch(int64_t id, Record* out, std::string* error = nullptr) const;
    // Loads every row (newest first) with its headers, for list restore.
    bool load_all(std::vector<Entry>* out, std::string* error = nullptr) const;
    // Deletes a row and its headers permanently.
    bool remove(int64_t id, std::string* error = nullptr);
    int64_t count(std::string* error = nullptr) const;

  private:
    bool exec(const char* sql, std::string* error) const;
    bool ensure_schema(std::string* error);
    bool ensure_column(const char* name, const char* type, std::string* error);
    bool read_headers(int64_t id, Record* out, std::string* error) const;
    bool write_headers(int64_t id, const std::vector<std::string>& headers,
                       std::string* error);
    void set_error(std::string* error, const std::string& message) const;

    sqlite3* db_ = nullptr;
    mutable std::string error_;
};

}  // namespace swiftdm
