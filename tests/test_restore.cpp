// CLI/backend test for the file resolution used when the download list is
// restored from history (no GUI, see AGENTS.md).

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "core/util.h"
#include "test_util.h"

namespace {

void write_file(const std::filesystem::path& path) {
    std::ofstream out(path);
    out << "data";
}

void set_mtime(const std::filesystem::path& path, int64_t unix_seconds) {
    const auto now = std::filesystem::file_time_type::clock::now();
    const auto system_now = std::chrono::system_clock::now();
    const auto delta = std::chrono::seconds(unix_seconds) -
                       std::chrono::duration_cast<std::chrono::seconds>(
                           system_now.time_since_epoch());
    std::filesystem::last_write_time(
        path, now + std::chrono::duration_cast<std::filesystem::file_time_type::duration>(
                      delta));
}

}  // namespace

int main() {
    namespace fs = std::filesystem;
    const std::string dir = testutil::make_temp_dir("restore");
    const int64_t near_unix = 1792399975;

    // Nothing in an empty folder.
    CHECK(swiftdm::resolve_existing_file(dir, "movie.mp4", near_unix).empty());

    // Exact name wins.
    write_file(fs::path(dir) / "movie.mp4");
    CHECK_EQ(swiftdm::resolve_existing_file(dir, "movie.mp4", near_unix),
             (fs::path(dir) / "movie.mp4").string());

    // A download saved with a unique suffix is found when the exact name is
    // gone, and the copy closest to the recorded attempt time is chosen.
    fs::remove(fs::path(dir) / "movie.mp4");
    write_file(fs::path(dir) / "movie (1).mp4");
    write_file(fs::path(dir) / "movie (2).mp4");
    set_mtime(fs::path(dir) / "movie (1).mp4", near_unix - 86400 * 30);
    set_mtime(fs::path(dir) / "movie (2).mp4", near_unix);
    CHECK_EQ(swiftdm::resolve_existing_file(dir, "movie.mp4", near_unix),
             (fs::path(dir) / "movie (2).mp4").string());

    // Unrelated names (different stem or extension) are not matched.
    CHECK(swiftdm::resolve_existing_file(dir, "other.mp4", near_unix).empty());
    CHECK(swiftdm::resolve_existing_file(dir, "movie.mkv", near_unix).empty());

    // A deleted file resolves to nothing (the "file missing" state).
    fs::remove(fs::path(dir) / "movie (1).mp4");
    fs::remove(fs::path(dir) / "movie (2).mp4");
    CHECK(swiftdm::resolve_existing_file(dir, "movie.mp4", near_unix).empty());

    // Missing or empty input is safe.
    CHECK(swiftdm::resolve_existing_file("", "movie.mp4", near_unix).empty());
    CHECK(swiftdm::resolve_existing_file(dir, "", near_unix).empty());

    testutil::cleanup_dir(dir);
    return testutil::finish("restore");
}
