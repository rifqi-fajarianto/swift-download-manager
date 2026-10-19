#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct sqlite3;

namespace swiftdm {

// SQLite-backed download history (schema in docs/spec/02-data-model.md).
//
// Rows are written when a download is added and updated while it runs, so the
// browser-sent page URL/title survive a restart. Restoring the list into the
// GUI at startup is a separate step and is not implemented yet.
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
        std::string mime_type;
        std::string error_text;
        std::string folder_path;
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
    bool fetch(int64_t id, Record* out, std::string* error = nullptr) const;
    int64_t count(std::string* error = nullptr) const;

  private:
    bool exec(const char* sql, std::string* error) const;
    bool ensure_schema(std::string* error);
    bool write_headers(int64_t id, const std::vector<std::string>& headers,
                       std::string* error);
    void set_error(std::string* error, const std::string& message) const;

    sqlite3* db_ = nullptr;
    mutable std::string error_;
};

}  // namespace swiftdm
