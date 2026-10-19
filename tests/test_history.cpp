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

    // A database written by an earlier release (no groupid column) is migrated
    // in place instead of failing on the first insert.
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
                "CREATE TABLE headers (id NUMERIC, header TEXT);";
            sqlite3_exec(legacy, legacy_schema, nullptr, nullptr, nullptr);
            sqlite3_close(legacy);
        }
    }
    swiftdm::HistoryStore legacy_store;
    check(legacy_store.open(legacy_path, &error), "an older database opens and is migrated");
    const int64_t legacy_id = legacy_store.insert(record, &error);
    check(legacy_id > 0, "the migrated database accepts new rows");
    check(legacy_store.fetch(legacy_id, &loaded, &error) && loaded.group_id == record.group_id,
          "the appended column round-trips after migration");

    fs::remove_all(dir, ec);
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("history tests passed\n");
    return 0;
}
