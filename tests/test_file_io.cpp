#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>

#include "core/file_io.h"
#include "core/util.h"
#include "core/win_headers.h"
#include "test_util.h"

using namespace swiftdm;

int main() {
    const std::string directory = testutil::make_temp_dir("file-io");
    const std::string path = path_to_utf8(path_from_utf8(directory) / "positioned-日本語.bin");
    CHECK(ensure_directory(directory));

    const int fd = open_file(path, FileRead | FileWrite | FileCreate | FileTruncate);
    CHECK(fd >= 0);
    if (fd >= 0) {
        CHECK(resize_file(fd, 12) == 0);
        CHECK(write_file_at(fd, "world", 5, 6) == 5);
        CHECK(write_file_at(fd, "hello ", 6, 0) == 6);
        CHECK(sync_file(fd) == 0);
        CHECK(close_file(fd) == 0);
    }

    std::ifstream input(path_from_utf8(path), std::ios::binary);
    const std::string contents((std::istreambuf_iterator<char>(input)),
                               std::istreambuf_iterator<char>());
    CHECK_EQ(contents, std::string("hello world\0", 12));

#ifdef _WIN32
    // Extending a part file must not zero-fill it: that stalls large downloads
    // before the first byte from the network arrives.
    const std::string sparse_path = path_to_utf8(path_from_utf8(directory) / "preallocated.bin");
    const std::uint64_t size = 512ull * 1024 * 1024;
    const int sparse_fd = open_file(sparse_path, FileRead | FileWrite | FileCreate | FileTruncate);
    CHECK(sparse_fd >= 0);
    if (sparse_fd >= 0) {
        IO_COUNTERS before{};
        IO_COUNTERS after{};
        CHECK(::GetProcessIoCounters(::GetCurrentProcess(), &before) != 0);
        CHECK(resize_file(sparse_fd, size) == 0);
        CHECK(::GetProcessIoCounters(::GetCurrentProcess(), &after) != 0);
        CHECK(static_cast<std::uint64_t>(after.WriteTransferCount - before.WriteTransferCount) <
              size / 4);
        CHECK(::_filelengthi64(sparse_fd) == static_cast<__int64>(size));
        CHECK(close_file(sparse_fd) == 0);

        std::ifstream tail(path_from_utf8(sparse_path), std::ios::binary);
        tail.seekg(static_cast<std::streamoff>(size) - 1);
        char last = 'x';
        tail.get(last);
        CHECK(tail.good());
        CHECK_EQ(static_cast<int>(last), 0);
    }
#endif

    testutil::cleanup_dir(directory);
    return testutil::finish("file_io");
}
