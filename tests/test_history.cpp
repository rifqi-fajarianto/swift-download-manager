// CLI/backend test for the SQLite history store (no GUI, see AGENTS.md).

#include <cstdio>
#include <filesystem>

#include <sqlite3.h>
#include <string>

#include "store/history.h"

namespace {

int failures = 0;

void check(bool condition, const char* description) {
    if (!condition) {
        std::printf("FAIL %s\n", description);
        ++failures;
    }
}

}  // namespace

int main() {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "swiftdm-history-test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    const std::string db_path = (dir / "test.db").string();

    swiftdm::HistoryStore store;
    std::string error;
    check(store.open(db_path, &error), "history database opens");
    check(store.is_open(), "store reports open");
    check(fs::exists(db_path), "database file is created");

    swiftdm::HistoryStore::Record record;
    record.url = "https://example.com/file.zip";
    record.method = "POST";
    record.filename = "file.zip";
    record.category = "Compressed";
    record.status = "Downloading";
    record.page_url = "https://example.com/page";
    record.page_title = "Example page";
    record.group_id = "pair-1f2e3d4c";
    record.file_size = 1048576;
    record.done = 262144;
    record.bandwidth_limit_kbps = 512;
    record.bandwidth_limit_enabled = true;
    record.folder_path = "/home/user/Downloads/Compressed";
    record.folder_exact = true;
    record.temp_path = "/tmp/swiftdm/download-abc";
    record.post_data = "q=1";
    record.headers = {"x-api-key: secret"};
    const int64_t id = store.insert(record, &error);
    check(id > 0, "insert returns a row id");
    check(store.count(&error) == 1, "one row is stored");

    swiftdm::HistoryStore::Record loaded;
    check(store.fetch(id, &loaded, &error), "fetch returns the row");
    check(loaded.url == record.url && loaded.page_url == record.page_url &&
              loaded.page_title == record.page_title && loaded.group_id == record.group_id,
          "URL and page metadata round-trip");
    check(loaded.post_data == "q=1" && loaded.method == "POST", "POST data round-trips");
    check(loaded.headers.size() == 1 && loaded.headers[0] == "x-api-key: secret",
          "extra headers round-trip");
    check(loaded.first_try > 0 && loaded.last_try > 0, "timestamps are filled in");
    check(loaded.done == record.done, "progress round-trips");
    check(loaded.temp_path == record.temp_path, "temp path round-trips");
    check(loaded.folder_path == record.folder_path && loaded.folder_exact,
          "exact destination folder round-trips");
    check(loaded.bandwidth_limit_kbps == 512 && loaded.bandwidth_limit_enabled,
          "speed limiter toggle and value round-trip");
    check(loaded.queue_id == 0, "a new row belongs to the main Downloads list");

    // Queue CRUD and the owning queue column. A row keeps its headers and
    // options when it moves between the main list and a queue.
    swiftdm::HistoryStore::QueueRecord queue;
    queue.name = "Batch";
    queue.max_simultaneous = 3;
    queue.connections = 4;
    queue.bandwidth_limit_enabled = true;
    queue.bandwidth_limit_kbps = 2048;
    queue.autostart = true;
    queue.notify_item = true;
    queue.notify_complete = true;
    queue.shutdown_when_complete = true;
    queue.shutdown_delay = 30;
    const int64_t queue_id = store.insert_queue(queue, &error);
    check(queue_id > 0, "insert_queue returns a queue id");

    std::vector<swiftdm::HistoryStore::QueueRecord> queues;
    check(store.load_queues(&queues, &error) && queues.size() == 1,
          "load_queues returns the stored queue");
    if (!queues.empty()) {
        const swiftdm::HistoryStore::QueueRecord& stored = queues.front();
        check(stored.id == queue_id && stored.name == "Batch",
              "the queue name and id round-trip");
        check(stored.max_simultaneous == 3 && stored.connections == 4,
              "the queue concurrency settings round-trip");
        check(stored.bandwidth_limit_enabled && stored.bandwidth_limit_kbps == 2048,
              "the queue speed limit round-trips");
        check(stored.autostart && stored.notify_item && stored.notify_complete,
              "the queue behavior toggles round-trip");
        check(stored.shutdown_when_complete && stored.shutdown_delay == 30,
              "the shutdown option and delay round-trip");
    }

    queue.id = queue_id;
    queue.name = "Renamed";
    queue.max_simultaneous = 0;  // Unlimited
    queue.connections = 0;
    queue.bandwidth_limit_enabled = false;
    queue.autostart = false;
    queue.notify_item = false;
    queue.notify_complete = false;
    queue.shutdown_when_complete = false;
    check(store.update_queue(queue, &error), "update_queue succeeds");
    check(store.load_queues(&queues, &error) && queues.size() == 1 &&
              queues.front().name == "Renamed" &&
              queues.front().max_simultaneous == 0 &&
              queues.front().connections == 0 && !queues.front().autostart &&
              !queues.front().bandwidth_limit_enabled,
          "updated queue settings are visible");

    check(store.update_queue_id(id, queue_id, "Queued", &error),
          "a row can move into a queue");
    check(store.fetch(id, &loaded, &error) && loaded.queue_id == queue_id &&
              loaded.status == "Queued",
          "the owning queue and status round-trip");
    check(loaded.headers.size() == 1 && loaded.headers[0] == "x-api-key: secret",
          "moving a row keeps its stored headers");
    check(loaded.post_data == "q=1" && loaded.bandwidth_limit_kbps == 512 &&
              loaded.bandwidth_limit_enabled,
          "moving a row keeps its options and speed limit");

    check(store.update_queue_id(id, 0, "Paused", &error),
          "a row can move back to the main list");
    check(store.fetch(id, &loaded, &error) && loaded.queue_id == 0 &&
              loaded.status == "Paused",
          "the row leaves the queue cleanly");

    check(store.remove_queue(queue_id, &error), "remove_queue succeeds");
    check(store.load_queues(&queues, &error) && queues.empty(),
          "removing a queue deletes only the queue record");
    check(store.count(&error) == 1, "removing a queue keeps its downloads");

    check(store.update_progress(id, 524288, 0, &error), "progress update succeeds");
    check(store.fetch(id, &loaded, &error) && loaded.done == 524288 &&
              loaded.file_size == record.file_size,
          "progress update keeps the known size");

    // A second row exercises list loading and ordering (newest first).
    swiftdm::HistoryStore::Record older = record;
    older.url = "https://example.com/older.zip";
    older.filename = "older.zip";
    older.status = "Complete";
    older.done = older.file_size;
    older.headers = {"x-one: 1", "x-two: 2"};
    const int64_t older_id = store.insert(older, &error);
    check(older_id > id, "second row gets a higher id");

    std::vector<swiftdm::HistoryStore::Entry> entries;
    check(store.load_all(&entries, &error), "load_all succeeds");
    check(entries.size() == 2, "load_all returns every row");
    check(!entries.empty() && entries.front().id == older_id, "load_all is newest first");
    if (!entries.empty()) {
        check(entries.front().record.headers.size() == 2,
              "load_all includes the per-row headers");
        check(entries.front().record.done == older.file_size,
              "load_all includes progress");
    }

    check(store.remove(older_id, &error), "remove succeeds");
    check(store.count(&error) == 1, "remove deletes the row");
    check(!store.fetch(older_id, &loaded, &error), "removed row cannot be fetched");
    check(store.load_all(&entries, &error) && entries.size() == 1,
          "load_all reflects the deletion");
    {
        sqlite3* raw = nullptr;
        int headers = -1;
        if (sqlite3_open_v2(db_path.c_str(), &raw, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK) {
            sqlite3_stmt* stmt = nullptr;
            if (sqlite3_prepare_v2(raw, "SELECT COUNT(*) FROM headers WHERE id=?1", -1, &stmt,
                                   nullptr) == SQLITE_OK) {
                sqlite3_bind_int64(stmt, 1, older_id);
                if (sqlite3_step(stmt) == SQLITE_ROW) headers = sqlite3_column_int(stmt, 0);
                sqlite3_finalize(stmt);
            }
            sqlite3_close(raw);
        }
        check(headers == 0, "remove drops the header rows of that download");
    }

    check(store.update_status(id, "Complete", "", &error), "status update succeeds");
    check(store.fetch(id, &loaded, &error) && loaded.status == "Complete",
          "status update is visible");
    check(loaded.error_text.empty(), "no error text after a successful update");

    check(store.update_status(id, "Error", "HTTP/1.1 403 Forbidden", &error),
          "error status update succeeds");
    check(store.fetch(id, &loaded, &error) && loaded.status == "Error" &&
              loaded.error_text == "HTTP/1.1 403 Forbidden",
          "error text round-trips");

    // Reopening reuses the schema without dropping rows.
    store.close();
    check(store.open(db_path, &error), "reopen succeeds");
    check(store.count(&error) == 1, "reopen keeps the row");
    check(!store.fetch(id + 1000, &loaded, &error), "fetching a missing row fails safely");

    // A database written by an earlier release (no groupid/done columns) is
    // migrated in place instead of failing on the first insert.
    const std::string legacy_path = (dir / "legacy.db").string();
    {
        sqlite3* legacy = nullptr;
        if (sqlite3_open_v2(legacy_path.c_str(), &legacy,
                            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) == SQLITE_OK) {
            const char* legacy_schema =
                "CREATE TABLE downloads (id INTEGER PRIMARY KEY, url TEXT, method TEXT,"
                " filename TEXT, ltype TEXT, filesize INTEGER, category TEXT, status TEXT,"
                " bandwidthlimit INTEGER, connections INTEGER, lasttry INTEGER, firsttry INTEGER,"
                " useragent TEXT, resumable INTEGER, pageurl TEXT, pagetitle TEXT,"
                " mimetype TEXT, errortext TEXT, postdata TEXT, folderpath TEXT, temppath TEXT);"
                "CREATE TABLE headers (id NUMERIC, header TEXT);"
                "INSERT INTO downloads (id,url,filename,filesize,status,bandwidthlimit,"
                " folderpath)"
                " VALUES (1,'https://example.com/old.iso','old.iso',5000,'Complete',256,"
                " '/tmp');";
            sqlite3_exec(legacy, legacy_schema, nullptr, nullptr, nullptr);
            sqlite3_close(legacy);
        }
    }
    swiftdm::HistoryStore legacy_store;
    check(legacy_store.open(legacy_path, &error), "an older database opens and is migrated");
    check(legacy_store.fetch(1, &loaded, &error) && loaded.done == 5000,
          "completed legacy rows get their progress backfilled");
    check(loaded.folder_exact == false,
          "legacy rows keep the base-folder semantics after migration");
    check(loaded.bandwidth_limit_enabled,
          "legacy non-zero limits migrate to the toggle being on");
    check(loaded.queue_id == 0,
          "legacy rows are migrated into the main Downloads list");
    const int64_t legacy_id = legacy_store.insert(record, &error);
    check(legacy_id > 0, "the migrated database accepts new rows");
    check(legacy_store.fetch(legacy_id, &loaded, &error) &&
              loaded.group_id == record.group_id && loaded.done == record.done &&
              loaded.folder_exact,
          "the appended columns round-trip after migration");

    fs::remove_all(dir, ec);
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("history tests passed\n");
    return 0;
}
