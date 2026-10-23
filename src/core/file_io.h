#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace swiftdm {

enum FileOpenFlag : unsigned {
    FileRead = 1u << 0,
    FileWrite = 1u << 1,
    FileCreate = 1u << 2,
    FileTruncate = 1u << 3,
    FileAppend = 1u << 4,
};

int open_file(const std::string& path, unsigned flags, unsigned mode = 0644);
int close_file(int fd);
int sync_file(int fd);
int resize_file(int fd, uint64_t size);
int seek_file(int fd, uint64_t offset);
std::ptrdiff_t write_file(int fd, const void* data, size_t size);
std::ptrdiff_t write_file_at(int fd, const void* data, size_t size, uint64_t offset);

}  // namespace swiftdm
