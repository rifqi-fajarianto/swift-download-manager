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
    "bandwidthlimit_enabled" NUMERIC, -- speed-limiter toggle (1 = limit applies)
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
    "folderexact"     NUMERIC,
    "temppath"        TEXT,
    "groupid"         TEXT,
    "done"            NUMERIC,
    "queueid"         NUMERIC, -- 0/NULL = main Downloads list
    PRIMARY KEY("id")
);
CREATE TABLE IF NOT EXISTS headers (id NUMERIC, header TEXT);
CREATE TABLE IF NOT EXISTS "queues" (
    "id"                     INTEGER,
    "name"                   TEXT,
    "max_simultaneous"       NUMERIC,
    "connections"            NUMERIC,
    "bandwidthlimit_enabled" NUMERIC,
    "bandwidthlimit"         NUMERIC,
    "autostart"              NUMERIC,
    "notify_item"            NUMERIC,
    "notify_complete"        NUMERIC,
    "shutdown_when_complete" NUMERIC,
    "shutdown_delay"         NUMERIC,
    PRIMARY KEY("id")
);
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

// Column order shared by fetch() and load_all().
const char* kRecordColumns =
    "url, method, filename, filesize, category, status, bandwidthlimit, connections,"
    " lasttry, firsttry, useragent, resumable, pageurl, pagetitle, mimetype, errortext,"
    " postdata, folderpath, temppath, groupid, done, folderexact, bandwidthlimit_enabled,"
    " queueid";

HistoryStore::Record read_record(sqlite3_stmt* stmt, int base = 0) {
    HistoryStore::Record record;
    record.url = column_text(stmt, base + 0);
    record.method = column_text(stmt, base + 1);
    record.filename = column_text(stmt, base + 2);
    record.file_size = static_cast<uint64_t>(sqlite3_column_int64(stmt, base + 3));
    record.category = column_text(stmt, base + 4);
    record.status = column_text(stmt, base + 5);
    record.bandwidth_limit_kbps = static_cast<int>(sqlite3_column_int64(stmt, base + 6));
    record.connections = static_cast<int>(sqlite3_column_int64(stmt, base + 7));
    record.last_try = sqlite3_column_int64(stmt, base + 8);
    record.first_try = sqlite3_column_int64(stmt, base + 9);
    record.user_agent = column_text(stmt, base + 10);
    record.resumable = sqlite3_column_int64(stmt, base + 11) != 0;
    record.page_url = column_text(stmt, base + 12);
    record.page_title = column_text(stmt, base + 13);
    record.mime_type = column_text(stmt, base + 14);
    record.error_text = column_text(stmt, base + 15);
    record.post_data = column_text(stmt, base + 16);
    record.folder_path = column_text(stmt, base + 17);
    record.temp_path = column_text(stmt, base + 18);
    record.group_id = column_text(stmt, base + 19);
    record.done = static_cast<uint64_t>(sqlite3_column_int64(stmt, base + 20));
    record.folder_exact = sqlite3_column_int64(stmt, base + 21) != 0;
    // Rows written before the toggle column existed carry NULL here; a
    // non-zero limit used to mean "limiter on" (see the backfill in open()).
    if (sqlite3_column_type(stmt, base + 22) == SQLITE_NULL) {
        record.bandwidth_limit_enabled = record.bandwidth_limit_kbps > 0;
    } else {
        record.bandwidth_limit_enabled = sqlite3_column_int64(stmt, base + 22) != 0;
    }
    if (sqlite3_column_type(stmt, base + 23) != SQLITE_NULL) {
        record.queue_id = sqlite3_column_int64(stmt, base + 23);
    }
    return record;
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

// CREATE TABLE IF NOT EXISTS does not touch databases written by an older
// release, so columns added later are appended when they are missing (spec 02
// migration note). SQLite has no "ADD COLUMN IF NOT EXISTS".
bool HistoryStore::ensure_column(const char* name, const char* type, std::string* error) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "PRAGMA table_info(downloads)", -1, &stmt, nullptr) != SQLITE_OK) {
        set_error(error, sqlite3_errmsg(db_));
        return false;
    }
    bool found = false;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (column_text(stmt, 1) == name) {
            found = true;
            break;
        }
    }
    sqlite3_finalize(stmt);
    if (found) return true;
    const std::string sql =
        std::string("ALTER TABLE downloads ADD COLUMN \"") + name + "\" " + type;
    return exec(sql.c_str(), error);
}

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
    // Columns appended after the first release.
    for (const auto& column : {std::pair<const char*, const char*>("folderpath", "TEXT"),
                               std::pair<const char*, const char*>("temppath", "TEXT"),
                               std::pair<const char*, const char*>("groupid", "TEXT"),
                               std::pair<const char*, const char*>("done", "NUMERIC"),
                               std::pair<const char*, const char*>("folderexact", "NUMERIC"),
                               std::pair<const char*, const char*>("bandwidthlimit_enabled",
                                                                   "NUMERIC"),
                               std::pair<const char*, const char*>("queueid", "NUMERIC")}) {
        if (!ensure_column(column.first, column.second, error)) {
            close();
            return false;
        }
    }
    // Backfill the progress column for completed rows written before 0.17.0.
    // Running it on every open is harmless: it only touches NULL values.
    if (!exec("UPDATE downloads SET done = filesize"
              " WHERE done IS NULL AND status = 'Complete' AND filesize > 0",
              error)) {
        close();
        return false;
    }
    // Rows written before the speed-limiter toggle existed carry NULL: any
    // non-zero BandWidthLimit used to mean the limiter was on. Explicit 0/1
    // values written by newer releases are never touched.
    if (!exec("UPDATE downloads SET bandwidthlimit_enabled ="
              " CASE WHEN bandwidthlimit > 0 THEN 1 ELSE 0 END"
              " WHERE bandwidthlimit_enabled IS NULL",
              error)) {
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
        " pagetitle, mimetype, errortext, postdata, folderpath, temppath, groupid, done,"
        " folderexact, bandwidthlimit_enabled, queueid)"
        " VALUES (?1,?2,?3,'',?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18,?19,"
        "?20,?21,?22,?23,?24)";
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
    bind_text(stmt, 20, record.group_id);
    bind_int64(stmt, 21, static_cast<int64_t>(record.done));
    bind_int64(stmt, 22, record.folder_exact ? 1 : 0);
    bind_int64(stmt, 23, record.bandwidth_limit_enabled ? 1 : 0);
    bind_int64(stmt, 24, record.queue_id);
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
        " postdata=?16, folderpath=?17, temppath=?18, groupid=?19, done=?20,"
        " folderexact=?21, bandwidthlimit_enabled=?22, queueid=?23 WHERE id=?24";
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
    bind_text(stmt, 19, record.group_id);
    bind_int64(stmt, 20, static_cast<int64_t>(record.done));
    bind_int64(stmt, 21, record.folder_exact ? 1 : 0);
    bind_int64(stmt, 22, record.bandwidth_limit_enabled ? 1 : 0);
    bind_int64(stmt, 23, record.queue_id);
    bind_int64(stmt, 24, id);
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

bool HistoryStore::update_progress(int64_t id, uint64_t done, uint64_t file_size,
                                   std::string* error) {
    if (!db_) {
        set_error(error, "history store is not open");
        return false;
    }
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_,
                           "UPDATE downloads SET done=?1,"
                           " filesize=CASE WHEN ?2>0 THEN ?2 ELSE filesize END"
                           " WHERE id=?3",
                           -1, &stmt, nullptr) != SQLITE_OK) {
        set_error(error, sqlite3_errmsg(db_));
        return false;
    }
    bind_int64(stmt, 1, static_cast<int64_t>(done));
    bind_int64(stmt, 2, static_cast<int64_t>(file_size));
    bind_int64(stmt, 3, id);
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
    const std::string sql = std::string("SELECT ") + kRecordColumns +
                            " FROM downloads WHERE id=?1";
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
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
    *out = read_record(stmt);
    sqlite3_finalize(stmt);

    if (!read_headers(id, out, error)) return false;
    return true;
}

bool HistoryStore::load_all(std::vector<Entry>* out, std::string* error) const {
    if (!db_ || !out) {
        set_error(error, "history store is not open");
        return false;
    }
    out->clear();
    sqlite3_stmt* stmt = nullptr;
    const std::string sql = std::string("SELECT id, ") + kRecordColumns +
                            " FROM downloads ORDER BY lasttry DESC, id DESC";
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        set_error(error, sqlite3_errmsg(db_));
        return false;
    }
    int rc = SQLITE_ROW;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        Entry entry;
        entry.id = sqlite3_column_int64(stmt, 0);
        entry.record = read_record(stmt, 1);
        out->push_back(std::move(entry));
    }
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        set_error(error, sqlite3_errmsg(db_));
        out->clear();
        return false;
    }
    for (Entry& entry : *out) {
        if (!read_headers(entry.id, &entry.record, error)) {
            out->clear();
            return false;
        }
    }
    return true;
}

bool HistoryStore::remove(int64_t id, std::string* error) {
    if (!db_) {
        set_error(error, "history store is not open");
        return false;
    }
    if (!exec("BEGIN IMMEDIATE", error)) return false;
    for (const char* sql : {"DELETE FROM headers WHERE id=?1",
                            "DELETE FROM downloads WHERE id=?1"}) {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            set_error(error, sqlite3_errmsg(db_));
            exec("ROLLBACK", nullptr);
            return false;
        }
        bind_int64(stmt, 1, id);
        const int rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        if (rc != SQLITE_DONE) {
            set_error(error, sqlite3_errmsg(db_));
            exec("ROLLBACK", nullptr);
            return false;
        }
    }
    return exec("COMMIT", error);
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

bool HistoryStore::load_queues(std::vector<QueueRecord>* out, std::string* error) const {
    if (!db_ || !out) {
        set_error(error, "history store is not open");
        return false;
    }
    out->clear();
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT id, name, max_simultaneous, connections, bandwidthlimit_enabled,"
        " bandwidthlimit, autostart, notify_item, notify_complete,"
        " shutdown_when_complete, shutdown_delay FROM queues ORDER BY id";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        set_error(error, sqlite3_errmsg(db_));
        return false;
    }
    int rc = SQLITE_ROW;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        QueueRecord queue;
        queue.id = sqlite3_column_int64(stmt, 0);
        queue.name = column_text(stmt, 1);
        queue.max_simultaneous = static_cast<int>(sqlite3_column_int64(stmt, 2));
        queue.connections = static_cast<int>(sqlite3_column_int64(stmt, 3));
        queue.bandwidth_limit_enabled = sqlite3_column_int64(stmt, 4) != 0;
        queue.bandwidth_limit_kbps = static_cast<int>(sqlite3_column_int64(stmt, 5));
        queue.autostart = sqlite3_column_int64(stmt, 6) != 0;
        queue.notify_item = sqlite3_column_int64(stmt, 7) != 0;
        queue.notify_complete = sqlite3_column_int64(stmt, 8) != 0;
        queue.shutdown_when_complete = sqlite3_column_int64(stmt, 9) != 0;
        queue.shutdown_delay = static_cast<int>(sqlite3_column_int64(stmt, 10));
        if (queue.max_simultaneous < 0) queue.max_simultaneous = 1;
        if (queue.connections < 0) queue.connections = 0;
        if (queue.bandwidth_limit_kbps < 0) queue.bandwidth_limit_kbps = 0;
        if (queue.shutdown_delay <= 0) queue.shutdown_delay = 60;
        out->push_back(std::move(queue));
    }
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        set_error(error, sqlite3_errmsg(db_));
        out->clear();
        return false;
    }
    return true;
}

int64_t HistoryStore::insert_queue(const QueueRecord& queue, std::string* error) {
    if (!db_) {
        set_error(error, "history store is not open");
        return 0;
    }
    static const char* sql =
        "INSERT INTO queues (name, max_simultaneous, connections, bandwidthlimit_enabled,"
        " bandwidthlimit, autostart, notify_item, notify_complete, shutdown_when_complete,"
        " shutdown_delay) VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10)";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        set_error(error, sqlite3_errmsg(db_));
        return 0;
    }
    bind_text(stmt, 1, queue.name);
    bind_int64(stmt, 2, queue.max_simultaneous);
    bind_int64(stmt, 3, queue.connections);
    bind_int64(stmt, 4, queue.bandwidth_limit_enabled ? 1 : 0);
    bind_int64(stmt, 5, queue.bandwidth_limit_kbps);
    bind_int64(stmt, 6, queue.autostart ? 1 : 0);
    bind_int64(stmt, 7, queue.notify_item ? 1 : 0);
    bind_int64(stmt, 8, queue.notify_complete ? 1 : 0);
    bind_int64(stmt, 9, queue.shutdown_when_complete ? 1 : 0);
    bind_int64(stmt, 10, queue.shutdown_delay);
    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        set_error(error, sqlite3_errmsg(db_));
        return 0;
    }
    return sqlite3_last_insert_rowid(db_);
}

bool HistoryStore::update_queue(const QueueRecord& queue, std::string* error) {
    if (!db_ || queue.id <= 0) {
        set_error(error, "history store is not open");
        return false;
    }
    static const char* sql =
        "UPDATE queues SET name=?1, max_simultaneous=?2, connections=?3,"
        " bandwidthlimit_enabled=?4, bandwidthlimit=?5, autostart=?6, notify_item=?7,"
        " notify_complete=?8, shutdown_when_complete=?9, shutdown_delay=?10 WHERE id=?11";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        set_error(error, sqlite3_errmsg(db_));
        return false;
    }
    bind_text(stmt, 1, queue.name);
    bind_int64(stmt, 2, queue.max_simultaneous);
    bind_int64(stmt, 3, queue.connections);
    bind_int64(stmt, 4, queue.bandwidth_limit_enabled ? 1 : 0);
    bind_int64(stmt, 5, queue.bandwidth_limit_kbps);
    bind_int64(stmt, 6, queue.autostart ? 1 : 0);
    bind_int64(stmt, 7, queue.notify_item ? 1 : 0);
    bind_int64(stmt, 8, queue.notify_complete ? 1 : 0);
    bind_int64(stmt, 9, queue.shutdown_when_complete ? 1 : 0);
    bind_int64(stmt, 10, queue.shutdown_delay);
    bind_int64(stmt, 11, queue.id);
    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        set_error(error, sqlite3_errmsg(db_));
        return false;
    }
    return true;
}

bool HistoryStore::remove_queue(int64_t id, std::string* error) {
    if (!db_) {
        set_error(error, "history store is not open");
        return false;
    }
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "DELETE FROM queues WHERE id=?1", -1, &stmt, nullptr) !=
        SQLITE_OK) {
        set_error(error, sqlite3_errmsg(db_));
        return false;
    }
    bind_int64(stmt, 1, id);
    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        set_error(error, sqlite3_errmsg(db_));
        return false;
    }
    return true;
}

bool HistoryStore::update_queue_id(int64_t id, int64_t queue_id, const std::string& status,
                                   std::string* error) {
    if (!db_) {
        set_error(error, "history store is not open");
        return false;
    }
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "UPDATE downloads SET queueid=?1, status=?2 WHERE id=?3", -1,
                           &stmt, nullptr) != SQLITE_OK) {
        set_error(error, sqlite3_errmsg(db_));
        return false;
    }
    bind_int64(stmt, 1, queue_id);
    bind_text(stmt, 2, status);
    bind_int64(stmt, 3, id);
    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        set_error(error, sqlite3_errmsg(db_));
        return false;
    }
    return true;
}

bool HistoryStore::read_headers(int64_t id, Record* out, std::string* error) const {
    sqlite3_stmt* stmt = nullptr;
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
