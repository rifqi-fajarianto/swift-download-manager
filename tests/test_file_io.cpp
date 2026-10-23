#include <fstream>
#include <iterator>
#include <string>

#include "core/file_io.h"
#include "core/util.h"
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

    testutil::cleanup_dir(directory);
    return testutil::finish("file_io");
}
