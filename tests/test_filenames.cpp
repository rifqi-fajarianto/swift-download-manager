#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sys/stat.h>
#include <string>
#include <system_error>

#include "core/util.h"
#include "engine/download_engine.h"
#include "store/settings.h"
#include "test_util.h"

using namespace swiftdm;

namespace {

// A name of the length a server can send: a token in the URL path.
std::string long_token(size_t length, char c = 'a') { return std::string(length, c); }

bool is_valid_utf8(const std::string& text) {
    size_t i = 0;
    while (i < text.size()) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        size_t extra = 0;
        if (c < 0x80) {
            extra = 0;
        } else if ((c & 0xE0) == 0xC0) {
            extra = 1;
        } else if ((c & 0xF0) == 0xE0) {
            extra = 2;
        } else if ((c & 0xF8) == 0xF0) {
            extra = 3;
        } else {
            return false;
        }
        if (i + extra >= text.size()) return false;
        for (size_t k = 1; k <= extra; ++k) {
            if ((static_cast<unsigned char>(text[i + k]) & 0xC0) != 0x80) return false;
        }
        i += extra + 1;
    }
    return true;
}

bool is_hex(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

void test_limit_filename_length() {
    // A short name is not touched.
    CHECK_EQ(limit_filename_length("video.mp4"), std::string("video.mp4"));
    CHECK_EQ(limit_filename_length("video.mp4", 240).size(), size_t(9));

    // A long name is shortened to the limit of a path component.
    const std::string long_name = long_token(700) + ".mp4";
    const std::string shortened = limit_filename_length(long_name);
    CHECK(shortened.size() <= kMaxFileNameBytes);
    CHECK(ends_with(shortened, ".mp4"));

    // The hash of the original name keeps the result stable (a resumed
    // download keeps the same temporary file) and keeps two different names
    // apart.
    CHECK_EQ(shortened, limit_filename_length(long_name));
    CHECK(shortened != limit_filename_length(long_token(701) + ".mp4"));

    // The hash is 16 hexadecimal digits, preceded by a dash.
    const size_t hash_start = shortened.size() - 4 - 17;
    CHECK_EQ(shortened[hash_start], '-');
    for (size_t i = hash_start + 1; i < hash_start + 17; ++i) {
        CHECK(is_hex(shortened[i]));
    }
}

void test_utf8_is_never_split() {
    // A name of multi-byte characters must not be cut inside a sequence.
    std::string name;
    while (name.size() < 700) name += "\xc3\xa4";  // "ä"
    const std::string shortened = limit_filename_length(name);
    CHECK(shortened.size() <= kMaxFileNameBytes);
    CHECK(is_valid_utf8(shortened));

    const std::string sanitized = sanitize_filename(name);
    CHECK(sanitized.size() <= kMaxFileNameBytes);
    CHECK(is_valid_utf8(sanitized));
}

void test_sanitize_filename() {
    CHECK_EQ(sanitize_filename("video.mp4"), std::string("video.mp4"));
    CHECK_EQ(sanitize_filename("a/b\\c:d*e?f\"g<h>i|j"), std::string("a_b_c_d_e_f_g_h_i_j"));
    // Trailing dots and spaces are not allowed.
    CHECK_EQ(sanitize_filename("name. "), std::string("name"));
    CHECK_EQ(sanitize_filename("name..."), std::string("name"));

    // Windows reserves the device names, also with an extension.
    CHECK_EQ(sanitize_filename("CON"), std::string("_CON"));
    CHECK_EQ(sanitize_filename("con.mp4"), std::string("_con.mp4"));
    CHECK_EQ(sanitize_filename("NUL.txt"), std::string("_NUL.txt"));
    CHECK_EQ(sanitize_filename("COM1"), std::string("_COM1"));
    CHECK_EQ(sanitize_filename("lpt9.log"), std::string("_lpt9.log"));
    CHECK_EQ(sanitize_filename("PRN"), std::string("_PRN"));
    // A name that only starts like a device name is fine.
    CHECK_EQ(sanitize_filename("console"), std::string("console"));
    CHECK_EQ(sanitize_filename("CONSOLE.mp4"), std::string("CONSOLE.mp4"));

    // A long name is shortened, the extension is kept.
    const std::string shortened = sanitize_filename(long_token(700) + ".mp4");
    CHECK(shortened.size() <= kMaxFileNameBytes);
    CHECK(ends_with(shortened, ".mp4"));
}

void test_file_name_from_url() {
    CHECK_EQ(file_name_from_url("https://example.com/video.mp4"), std::string("video.mp4"));
    CHECK_EQ(file_name_from_url("https://example.com/a/b/"), std::string("b"));

    // A URL with a very long path segment must not produce a name that is
    // too long for the filesystem.
    const std::string url = "https://example.com/api/videos/" + long_token(700);
    const std::string name = file_name_from_url(url);
    CHECK(name.size() <= kMaxFileNameBytes);
    CHECK(is_valid_utf8(name));
}

void test_workspace_with_long_name(const std::string& dir) {
    Settings settings;
    settings.temp_dir = dir + "/tmp";

    const DownloadWorkspace ws =
        make_workspace(settings, "https://example.com/api/videos/long", long_token(700));
    const std::string part_name = std::filesystem::path(ws.part_path).filename().string();
    CHECK(part_name.size() <= kMaxFileNameBytes);

    std::error_code ec;
    CHECK(ensure_private_directory(settings.temp_dir, nullptr));
    CHECK(ensure_private_directory(ws.dir, nullptr));
    struct stat parent_stat {};
    CHECK(::stat(settings.temp_dir.c_str(), &parent_stat) == 0);
    CHECK((parent_stat.st_mode & 0777) == 0700);
    struct stat workspace_stat {};
    CHECK(::stat(ws.dir.c_str(), &workspace_stat) == 0);
    CHECK((workspace_stat.st_mode & 0777) == 0700);
    // The temporary file has to be creatable. It used to fail here with
    // "File name too long".
    const int fd = ::open(ws.part_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0);
    if (fd >= 0) ::close(fd);

    // A short name keeps the plain "<name>.part" file.
    const DownloadWorkspace short_ws =
        make_workspace(settings, "https://example.com/short", "video.mp4");
    CHECK(ends_with(short_ws.part_path, "video.mp4.part"));
}

void test_unique_path_stays_in_limit(const std::string& dir) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);

    const std::string name = limit_filename_length(long_token(250, 'b') + ".mp4");
    CHECK(name.size() <= kMaxFileNameBytes);
    const std::filesystem::path target = std::filesystem::path(dir) / name;
    {
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        out << "x";
    }

    // The second file of the same name gets the " (1)" suffix, and it stays
    // within the limit of a path component.
    const std::string unique = unique_path(target.string());
    const std::string unique_name = std::filesystem::path(unique).filename().string();
    CHECK(unique_name.size() <= kMaxFileNameBytes);
    CHECK(unique != target.string());
    CHECK(unique_name.find(" (1).mp4") != std::string::npos);
}

// The final name is claimed atomically: an existing file is never replaced,
// even when a file appears after the name was chosen (audit finding 7).
void test_final_name_never_overwrites(const std::string& dir) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);

    const std::string first_source = dir + "/first.part";
    {
        std::ofstream out(first_source, std::ios::binary | std::ios::trunc);
        out << "first";
    }
    std::string final_path;
    std::string error;
    CHECK(move_file_unique(first_source, dir, "file.bin", &final_path, &error));
    CHECK_EQ(final_path, dir + "/file.bin");
    CHECK(!std::filesystem::exists(first_source));
    {
        std::ifstream in(final_path);
        std::string content;
        in >> content;
        CHECK_EQ(content, std::string("first"));
    }

    // The plain name is taken: the next free name is claimed and the existing
    // file keeps its content.
    const std::string second_source = dir + "/second.part";
    {
        std::ofstream out(second_source, std::ios::binary | std::ios::trunc);
        out << "second";
    }
    CHECK(move_file_unique(second_source, dir, "file.bin", &final_path, &error));
    CHECK_EQ(final_path, dir + "/file (1).bin");
    {
        std::ifstream in(dir + "/file.bin");
        std::string content;
        in >> content;
        CHECK_EQ(content, std::string("first"));
    }

    // A gap in the numbering is not reused: " (1)" is taken, so " (2)" is next.
    {
        std::ofstream out(dir + "/file (2).bin", std::ios::binary | std::ios::trunc);
        out << "existing";
    }
    const std::string third_source = dir + "/third.part";
    {
        std::ofstream out(third_source, std::ios::binary | std::ios::trunc);
        out << "third";
    }
    CHECK(move_file_unique(third_source, dir, "file.bin", &final_path, &error));
    CHECK_EQ(final_path, dir + "/file (3).bin");
    {
        std::ifstream in(dir + "/file (2).bin");
        std::string content;
        in >> content;
        CHECK_EQ(content, std::string("existing"));
    }
}

}  // namespace

int main() {
    const std::string dir = testutil::make_temp_dir("filenames");

    test_limit_filename_length();
    test_utf8_is_never_split();
    test_sanitize_filename();
    test_file_name_from_url();
    test_workspace_with_long_name(dir);
    test_unique_path_stays_in_limit(dir + "/unique");
    test_final_name_never_overwrites(dir + "/final-name");

    testutil::cleanup_dir(dir);
    return testutil::finish("filenames");
}
