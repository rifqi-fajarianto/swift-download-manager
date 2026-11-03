#include "core/instance_lock.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>

#ifdef _WIN32
#include "core/win_headers.h"
#include <process.h>
#define getpid _getpid
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "core/file_io.h"
#include "core/util.h"

namespace swiftdm {
namespace {

constexpr char kRoleGui[] = "gui";
constexpr char kRoleCli[] = "cli";

// The owner record lives at byte 0; the exclusive byte-range lock is taken far
// past it so the record is never inside the locked region.
constexpr unsigned long kLockOffsetLow = 4096;
constexpr unsigned long kLockOffsetHigh = 0;

const char* role_token(InstanceRole role) {
    return role == InstanceRole::Gui ? kRoleGui : kRoleCli;
}

// Directory that holds the lock file and the socket. POSIX prefers the per-user
// runtime directory and falls back to a private directory under the temporary
// directory when the session has none (a bare `swiftdm get` over ssh, for
// example). Windows uses the local data directory, which is already per user.
std::string state_dir() {
#ifdef _WIN32
    return xdg_data_home() + "\\SwiftDM";
#else
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    if (runtime && runtime[0] == '/') {
        std::error_code ec;
        if (std::filesystem::is_directory(path_from_utf8(runtime), ec)) return runtime;
    }
    // The temporary directory is shared by every user on the machine, so the
    // fallback lives in an owner-only subdirectory instead of directly in it.
    const std::string private_dir =
        "/tmp/swiftdm-" + std::to_string(static_cast<long long>(::getuid()));
    ensure_private_directory(private_dir);
    return private_dir;
#endif
}

}  // namespace

InstanceLock::~InstanceLock() { release(); }

std::string InstanceLock::name_for_config(std::string_view config_path) {
    // The hash keeps the name short and filesystem friendly; the config path
    // makes the endpoint follow XDG_CONFIG_HOME and the Windows %APPDATA%.
    return "swiftdm-" + hex_hash64(fnv1a_hash(config_path));
}

std::string InstanceLock::lock_path(const std::string& name) {
#ifdef _WIN32
    return state_dir() + "\\" + name + ".lock";
#else
    return state_dir() + "/" + name + ".lock";
#endif
}

std::string InstanceLock::endpoint_path(const std::string& name) {
#ifdef _WIN32
    // QLocalServer maps the name to \\.\pipe\<name>; every Windows session has
    // its own pipe namespace, so the endpoint is already per user.
    return name;
#else
    return state_dir() + "/" + name + ".sock";
#endif
}

bool InstanceLock::acquire(InstanceRole role, const std::string& name,
                           std::string* error) {
    if (held_) return true;
    if (error) error->clear();

    path_ = lock_path(name);
    role_ = role;

    std::error_code ec;
    const std::filesystem::path file = path_from_utf8(path_);
    std::filesystem::create_directories(file.parent_path(), ec);
    if (ec) {
        if (error) {
            *error = "cannot create '" + path_to_utf8(file.parent_path()) + "': " +
                     ec.message();
        }
        return false;
    }

#ifdef _WIN32
    HANDLE handle = ::CreateFileW(file.c_str(), GENERIC_READ | GENERIC_WRITE,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        if (error) *error = "cannot open '" + path_ + "'";
        return false;
    }
    OVERLAPPED overlapped = {};
    // The lock sits at a fixed offset past the owner record. Both are in the
    // same file, so one handle is enough, and keeping the record outside the
    // locked byte means the record stays readable no matter how strictly the
    // platform enforces byte-range locks.
    overlapped.Offset = kLockOffsetLow;
    overlapped.OffsetHigh = kLockOffsetHigh;
    if (!::LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0,
                      &overlapped)) {
        ::CloseHandle(handle);
        // The file stays in place: the holder owns it and reads its own record.
        if (error) error->clear();
        return false;
    }
    handle_ = handle;
#else
    // POSIX: the lock is taken on the inode, so a file that another process
    // replaced between our open() and flock() would give us a lock nobody else
    // can see. Verify that the path still points at the inode we locked and
    // retry otherwise.
    for (int attempt = 0; attempt < 8; ++attempt) {
        const int fd = ::open(path_.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (fd < 0) {
            if (error) *error = "cannot open '" + path_ + "': " + std::strerror(errno);
            return false;
        }
        if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
            ::close(fd);
            if (error) error->clear();
            return false;
        }
        struct stat locked = {};
        struct stat on_disk = {};
        if (::fstat(fd, &locked) == 0 && ::stat(path_.c_str(), &on_disk) == 0 &&
            locked.st_dev == on_disk.st_dev && locked.st_ino == on_disk.st_ino) {
            fd_ = fd;
            break;
        }
        // The path changed under us; the lock we hold is on a file nobody can
        // reach, so drop it and try the file that is there now.
        ::flock(fd, LOCK_UN);
        ::close(fd);
    }
    if (fd_ < 0) {
        if (error) *error = "cannot lock '" + path_ + "'";
        return false;
    }
#endif

    held_ = true;
    write_owner();
    return true;
}

void InstanceLock::release() {
    if (!held_) return;
    held_ = false;
    // The file is removed before the lock is dropped, and acquire() verifies
    // that the path still points at the inode it locked. Without that check a
    // process could lock an inode that was already unlinked while another
    // process created a fresh file at the same path and locked that one too.
#ifdef _WIN32
    if (handle_) {
        // Closing the handle releases the lock.
        ::CloseHandle(static_cast<HANDLE>(handle_));
        handle_ = nullptr;
        std::error_code ec;
        std::filesystem::remove(path_from_utf8(path_), ec);
    }
#else
    if (fd_ >= 0) {
        ::unlink(path_.c_str());
        ::flock(fd_, LOCK_UN);
        ::close(fd_);
        fd_ = -1;
    }
#endif
}

bool InstanceLock::write_owner() const {
    const std::string text =
        std::string(role_token(role_)) + " " +
        std::to_string(static_cast<long long>(::getpid())) + "\n";
#ifdef _WIN32
    if (!handle_) return false;
    HANDLE handle = static_cast<HANDLE>(handle_);
    // Truncate first: a longer record from an earlier run must not survive
    // behind the new one and be read as part of the process id.
    ::SetFilePointer(handle, 0, nullptr, FILE_BEGIN);
    ::SetEndOfFile(handle);
    DWORD written = 0;
    const BOOL ok = ::WriteFile(handle, text.data(), static_cast<DWORD>(text.size()),
                                &written, nullptr);
    ::FlushFileBuffers(handle);
    return ok != 0 && written == text.size();
#else
    if (fd_ < 0) return false;
    if (::ftruncate(fd_, 0) != 0) return false;
    if (::lseek(fd_, 0, SEEK_SET) < 0) return false;
    const ssize_t written = ::write(fd_, text.data(), text.size());
    return written == static_cast<ssize_t>(text.size());
#endif
}

InstanceOwner InstanceLock::owner() const {
    InstanceOwner info;
    // Read through the project's file layer: std::fopen() takes an ANSI path on
    // Windows, so a profile directory outside the ANSI code page would make the
    // record unreadable and the refusal message would guess the wrong role.
    const int fd = open_file(path_, FileRead);
    if (fd < 0) return info;
    char buffer[64] = {};
    const std::ptrdiff_t read = read_file(fd, buffer, sizeof(buffer) - 1);
    close_file(fd);
    if (read <= 0) return info;

    std::string text(buffer, static_cast<size_t>(read));
    const size_t end = text.find_first_of("\r\n");
    if (end != std::string::npos) text.resize(end);
    const size_t space = text.find(' ');
    const std::string role = space == std::string::npos ? text : text.substr(0, space);
    if (role != kRoleGui && role != kRoleCli) return info;
    info.role = role == kRoleCli ? InstanceRole::Cli : InstanceRole::Gui;
    if (space != std::string::npos) {
        try {
            info.pid = std::stoll(text.substr(space + 1));
        } catch (...) {
            info.pid = 0;
        }
    }
    info.known = true;
    return info;
}

InstanceOwner InstanceLock::owner_wait(int timeout_ms) const {
    InstanceOwner info = owner();
    if (info.known) return info;
    // The owner holds the lock but has not written its record yet. Wait a short
    // while instead of presenting a guess; the wait stays bounded so a crashed
    // owner that never wrote the record cannot hang the caller.
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        info = owner();
        if (info.known) return info;
    }
    return info;
}

}  // namespace swiftdm
