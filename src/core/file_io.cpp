#include "core/file_io.h"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <mutex>

#include "core/util.h"
#include "core/win_headers.h"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <limits.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace swiftdm {
namespace {

#ifdef _WIN32
std::mutex positioned_write_mutex;
#endif

int write_all(int fd, const void* data, size_t size) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    size_t written = 0;
    while (written < size) {
        const size_t chunk = std::min(size - written, static_cast<size_t>(INT_MAX));
#ifdef _WIN32
        const int rc = ::_write(fd, bytes + written, static_cast<unsigned>(chunk));
#else
        const ssize_t rc = ::write(fd, bytes + written, chunk);
#endif
        if (rc < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (rc == 0) {
            errno = EIO;
            return -1;
        }
        written += static_cast<size_t>(rc);
    }
    return 0;
}

}  // namespace

int open_file(const std::string& path, unsigned flags, unsigned mode) {
#ifdef _WIN32
    (void)mode;
    int native_flags = _O_BINARY;
    if ((flags & FileRead) && (flags & FileWrite)) native_flags |= _O_RDWR;
    else if (flags & FileWrite) native_flags |= _O_WRONLY;
    else native_flags |= _O_RDONLY;
    if (flags & FileCreate) native_flags |= _O_CREAT;
    if (flags & FileTruncate) native_flags |= _O_TRUNC;
    if (flags & FileAppend) native_flags |= _O_APPEND;
    const std::wstring wide = path_from_utf8(path).native();
    if (wide.empty() && !path.empty()) {
        errno = EINVAL;
        return -1;
    }
    return ::_wopen(wide.c_str(), native_flags, _S_IREAD | _S_IWRITE);
#else
    int native_flags = 0;
    if ((flags & FileRead) && (flags & FileWrite)) native_flags |= O_RDWR;
    else if (flags & FileWrite) native_flags |= O_WRONLY;
    else native_flags |= O_RDONLY;
    if (flags & FileCreate) native_flags |= O_CREAT;
    if (flags & FileTruncate) native_flags |= O_TRUNC;
    if (flags & FileAppend) native_flags |= O_APPEND;
#ifdef O_CLOEXEC
    native_flags |= O_CLOEXEC;
#endif
    return ::open(path.c_str(), native_flags, static_cast<mode_t>(mode));
#endif
}

int close_file(int fd) {
#ifdef _WIN32
    return ::_close(fd);
#else
    return ::close(fd);
#endif
}

int sync_file(int fd) {
#ifdef _WIN32
    return ::_commit(fd);
#else
    return ::fsync(fd);
#endif
}

int resize_file(int fd, uint64_t size) {
#ifdef _WIN32
    const errno_t rc = ::_chsize_s(fd, size);
    if (rc != 0) errno = static_cast<int>(rc);
    return rc == 0 ? 0 : -1;
#else
    return ::ftruncate(fd, static_cast<off_t>(size));
#endif
}

int seek_file(int fd, uint64_t offset) {
#ifdef _WIN32
    return ::_lseeki64(fd, static_cast<__int64>(offset), SEEK_SET) < 0 ? -1 : 0;
#else
    return ::lseek(fd, static_cast<off_t>(offset), SEEK_SET) < 0 ? -1 : 0;
#endif
}

std::ptrdiff_t write_file(int fd, const void* data, size_t size) {
    if (write_all(fd, data, size) != 0) return -1;
    return static_cast<std::ptrdiff_t>(size);
}

std::ptrdiff_t write_file_at(int fd, const void* data, size_t size, uint64_t offset) {
#ifdef _WIN32
    // The Windows CRT has no positioned-write primitive. Callbacks for one
    // libcurl multi handle are serialized, and this lock also protects any
    // future callers that share the descriptor across threads.
    std::lock_guard<std::mutex> lock(positioned_write_mutex);
    if (seek_file(fd, offset) != 0 || write_all(fd, data, size) != 0) return -1;
#else
    const auto* bytes = static_cast<const unsigned char*>(data);
    size_t written = 0;
    while (written < size) {
        const size_t chunk = std::min(size - written, static_cast<size_t>(SSIZE_MAX));
        const ssize_t rc = ::pwrite(fd, bytes + written, chunk,
                                    static_cast<off_t>(offset + written));
        if (rc < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (rc == 0) {
            errno = EIO;
            return -1;
        }
        written += static_cast<size_t>(rc);
    }
#endif
    return static_cast<std::ptrdiff_t>(size);
}

}  // namespace swiftdm
