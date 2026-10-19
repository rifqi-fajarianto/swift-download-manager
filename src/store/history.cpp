#include "store/history.h"

#include <sqlite3.h>

#include <ctime>
#include <filesystem>

#include "core/log.h"

namespace swiftdm {
namespace {

const char* kSchema = R"sql(
CREATE TABLE IF NOT EXISTS "downloads" (
    "id"              INTEGER,
    "url"             TEXT,
    "method"          TEXT,
    "filename"        TEXT,
    "ltype"           TEXT,
    "filesize"        NUMERIC,
    "category"        TEXT,
    "status"          TEXT,
    "bandwidthlimit"  NUMERIC,
    "connections"     NUMERIC,
    "lasttry"         NUMERIC,
    "firsttry"        NUMERIC,
    "useragent"       TEXT,
    "resumable"       NUMERIC,
    "pageurl"         TEXT,
    "pagetitle"       TEXT,
    "hittitle"        TEXT,
    "mimetype"        TEXT,
    "errortext"       TEXT,
    "urla"            TEXT,
    "postdata"        TEXT,
    "folderpath"      TEXT,
    "temppath"        TEXT,
    PRIMARY KEY("id")
);
CREATE TABLE IF NOT EXISTS headers (id NUMERIC, header TEXT);
)sql";

void bind_text(sqlite3_stmt* stmt, int index, const std::string& value) {
    sqlite3_bind_text(stmt, index, value.c_str(), static_cast<int>(value.size()),
                      SQLITE_TRANSIENT);
}

void bind_int64(sqlite3_stmt* stmt, int index, int64_t value) {
    sqlite3_bind_int64(stmt, index, value);
}

std::string column_text(sqlite3_stmt* stmt, int index) {
    const unsigned char* text = sqlite3_column_text(stmt, index);
    if (!text) return {};
    return reinterpret_cast<const char*>(text);
}

}  // namespace

HistoryStore::~HistoryStore() { close(); }

void HistoryStore::set_error(std::string* error, const std::string& message) const {
    error_ = message;
    if (error) *error = message;
    log_warn("history: " + message);
}

bool HistoryStore::exec(const char* sql, std::string* error) const {
    char* message = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &message) != SQLITE_OK) {
        const std::string text = message ? message : "sqlite error";
        sqlite3_free(message);
        set_error(error, text);
        return false;
    }
    return true;
}

bool HistoryStore::ensure_schema(std::string* error) { return exec(kSchema, error); }

bool HistoryStore::open(const std::string& path, std::string* error) {
    close();
    std::error_code ec;
    const std::filesystem::path file(path);
    if (file.has_parent_path()) {
        std::filesystem::create_directories(file.parent_path(), ec);
    }
    if (sqlite3_open_v2(path.c_str(), &db_,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK) {
        const std::string message = db_ ? sqlite3_errmsg(db_) : "cannot open database";
        if (db_) {
            sqlite3_close(db_);
            db_ = nullptr;
        }
        set_error(error, message);
        return false;
    }
    sqlite3_busy_timeout(db_, 2000);
    if (!ensure_schema(error)) {
        close();
        return false;
    }
    return true;
}

void HistoryStore::close() {
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

int64_t HistoryStore::insert(const Record& record, std::string* error) {
    if (!db_) {
        set_error(error, "history store is not open");
        return 0;
    }
    static const char* sql =
        "INSERT INTO downloads (url, method, filename, ltype, filesize, category, status,"
        " bandwidthlimit, connections, lasttry, firsttry, useragent, resumable, pageurl,"
        " pagetitle, mimetype, errortext, postdata, folderpath, temppath)"
        " VALUES (?1,?2,?3,'',?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18,?19)";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        set_error(error, sqlite3_errmsg(db_));
        return 0;
    }
    const int64_t now = static_cast<int64_t>(std::time(nullptr));
    bind_text(stmt, 1, record.url);
    bind_text(stmt, 2, record.method);
    bind_text(stmt, 3, record.filename);
    bind_int64(stmt, 4, static_cast<int64_t>(record.file_size));
    bind_text(stmt, 5, record.category);
    bind_text(stmt, 6, record.status);
    bind_int64(stmt, 7, record.bandwidth_limit_kbps);
    bind_int64(stmt, 8, record.connections);
    bind_int64(stmt, 9, record.last_try > 0 ? record.last_try : now);
    bind_int64(stmt, 10, record.first_try > 0 ? record.first_try : now);
    bind_text(stmt, 11, record.user_agent);
    bind_int64(stmt, 12, record.resumable ? 1 : 0);
    bind_text(stmt, 13, record.page_url);
    bind_text(stmt, 14, record.page_title);
    bind_text(stmt, 15, record.mime_type);
    bind_text(stmt, 16, record.error_text);
    bind_text(stmt, 17, record.post_data);
    bind_text(stmt, 18, record.folder_path);
    bind_text(stmt, 19, record.temp_path);
    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        set_error(error, sqlite3_errmsg(db_));
        return 0;
    }
    const int64_t id = sqlite3_last_insert_rowid(db_);
    if (!record.headers.empty() && !write_headers(id, record.headers, error)) return 0;
    return id;
}

bool HistoryStore::update(int64_t id, const Record& record, std::string* error) {
    if (!db_) {
        set_error(error, "history store is not open");
        return false;
    }
    static const char* sql =
        "UPDATE downloads SET url=?1, method=?2, filename=?3, filesize=?4, category=?5,"
        " status=?6, bandwidthlimit=?7, connections=?8, lasttry=?9, useragent=?10,"
        " resumable=?11, pageurl=?12, pagetitle=?13, mimetype=?14, errortext=?15,"
        " postdata=?16, folderpath=?17, temppath=?18 WHERE id=?19";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        set_error(error, sqlite3_errmsg(db_));
        return false;
    }
    bind_text(stmt, 1, record.url);
    bind_text(stmt, 2, record.method);
    bind_text(stmt, 3, record.filename);
    bind_int64(stmt, 4, static_cast<int64_t>(record.file_size));
    bind_text(stmt, 5, record.category);
    bind_text(stmt, 6, record.status);
    bind_int64(stmt, 7, record.bandwidth_limit_kbps);
    bind_int64(stmt, 8, record.connections);
    bind_int64(stmt, 9, record.last_try > 0 ? record.last_try
                                            : static_cast<int64_t>(std::time(nullptr)));
    bind_text(stmt, 10, record.user_agent);
    bind_int64(stmt, 11, record.resumable ? 1 : 0);
    bind_text(stmt, 12, record.page_url);
    bind_text(stmt, 13, record.page_title);
    bind_text(stmt, 14, record.mime_type);
    bind_text(stmt, 15, record.error_text);
    bind_text(stmt, 16, record.post_data);
    bind_text(stmt, 17, record.folder_path);
    bind_text(stmt, 18, record.temp_path);
    bind_int64(stmt, 19, id);
    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        set_error(error, sqlite3_errmsg(db_));
        return false;
    }
    return true;
}

bool HistoryStore::update_status(int64_t id, const std::string& status,
                                 const std::string& error_text, std::string* error) {
    if (!db_) {
        set_error(error, "history store is not open");
        return false;
    }
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_,
                           "UPDATE downloads SET status=?1, errortext=?2, lasttry=?3"
                           " WHERE id=?4",
                           -1, &stmt, nullptr) != SQLITE_OK) {
        set_error(error, sqlite3_errmsg(db_));
        return false;
    }
    bind_text(stmt, 1, status);
    bind_text(stmt, 2, error_text);
    bind_int64(stmt, 3, static_cast<int64_t>(std::time(nullptr)));
    bind_int64(stmt, 4, id);
    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        set_error(error, sqlite3_errmsg(db_));
        return false;
    }
    return true;
}

bool HistoryStore::fetch(int64_t id, Record* out, std::string* error) const {
    if (!db_ || !out) {
        set_error(error, "history store is not open");
        return false;
    }
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_,
                           "SELECT url, method, filename, filesize, category, status,"
                           " bandwidthlimit, connections, lasttry, firsttry, useragent,"
                           " resumable, pageurl, pagetitle, mimetype, errortext, postdata,"
                           " folderpath, temppath FROM downloads WHERE id=?1",
                           -1, &stmt, nullptr) != SQLITE_OK) {
        set_error(error, sqlite3_errmsg(db_));
        return false;
    }
    bind_int64(stmt, 1, id);
    const int rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        set_error(error, rc == SQLITE_DONE ? "download not found" : sqlite3_errmsg(db_));
        return false;
    }
    *out = Record{};
    out->url = column_text(stmt, 0);
    out->method = column_text(stmt, 1);
    out->filename = column_text(stmt, 2);
    out->file_size = static_cast<uint64_t>(sqlite3_column_int64(stmt, 3));
    out->category = column_text(stmt, 4);
    out->status = column_text(stmt, 5);
    out->bandwidth_limit_kbps = static_cast<int>(sqlite3_column_int64(stmt, 6));
    out->connections = static_cast<int>(sqlite3_column_int64(stmt, 7));
    out->last_try = sqlite3_column_int64(stmt, 8);
    out->first_try = sqlite3_column_int64(stmt, 9);
    out->user_agent = column_text(stmt, 10);
    out->resumable = sqlite3_column_int64(stmt, 11) != 0;
    out->page_url = column_text(stmt, 12);
    out->page_title = column_text(stmt, 13);
    out->mime_type = column_text(stmt, 14);
    out->error_text = column_text(stmt, 15);
    out->post_data = column_text(stmt, 16);
    out->folder_path = column_text(stmt, 17);
    out->temp_path = column_text(stmt, 18);
    sqlite3_finalize(stmt);

    if (sqlite3_prepare_v2(db_, "SELECT header FROM headers WHERE id=?1 ORDER BY rowid", -1,
                           &stmt, nullptr) != SQLITE_OK) {
        set_error(error, sqlite3_errmsg(db_));
        return false;
    }
    bind_int64(stmt, 1, id);
    while (sqlite3_step(stmt) == SQLITE_ROW) out->headers.push_back(column_text(stmt, 0));
    sqlite3_finalize(stmt);
    return true;
}

int64_t HistoryStore::count(std::string* error) const {
    if (!db_) {
        set_error(error, "history store is not open");
        return -1;
    }
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM downloads", -1, &stmt, nullptr) !=
        SQLITE_OK) {
        set_error(error, sqlite3_errmsg(db_));
        return -1;
    }
    int64_t total = -1;
    if (sqlite3_step(stmt) == SQLITE_ROW) total = sqlite3_column_int64(stmt, 0);
    else set_error(error, sqlite3_errmsg(db_));
    sqlite3_finalize(stmt);
    return total;
}

bool HistoryStore::write_headers(int64_t id, const std::vector<std::string>& headers,
                                 std::string* error) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "INSERT INTO headers (id, header) VALUES (?1, ?2)", -1, &stmt,
                           nullptr) != SQLITE_OK) {
        set_error(error, sqlite3_errmsg(db_));
        return false;
    }
    for (const std::string& header : headers) {
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
        bind_int64(stmt, 1, id);
        bind_text(stmt, 2, header);
        if (sqlite3_step(stmt) != SQLITE_DONE) {
            const std::string message = sqlite3_errmsg(db_);
            sqlite3_finalize(stmt);
            set_error(error, message);
            return false;
        }
    }
    sqlite3_finalize(stmt);
    return true;
}

}  // namespace swiftdm
