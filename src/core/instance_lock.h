#pragma once

#include <string>
#include <string_view>

namespace swiftdm {

// Which kind of run owns the instance: the GUI (plain start, --gui or the
// login entry --autostart) or a headless `swiftdm get` download.
enum class InstanceRole { Gui, Cli };

// The run that holds an InstanceLock, as written into the lock file. `known`
// is false when the file could not be read, so callers do not present a guess
// as a fact.
struct InstanceOwner {
    InstanceRole role = InstanceRole::Gui;
    long long pid = 0;
    bool known = false;
};

// Exclusive, per-user lock that keeps SwiftDM to a single process. Two runs
// would otherwise share the SQLite database, the temp workspaces and the
// browser endpoint, and a second tray icon would appear at login when the
// autostart entry and a shortcut start together.
//
// The lock is an advisory file lock (flock on POSIX, LockFileEx on Windows), so
// a crashed run never blocks the next start: the kernel drops the lock with the
// process. The file also records the owner's role and pid so a second start can
// tell which run is in the way.
class InstanceLock {
  public:
    InstanceLock() = default;
    ~InstanceLock();
    InstanceLock(const InstanceLock&) = delete;
    InstanceLock& operator=(const InstanceLock&) = delete;

    // The lock follows the configuration path, so separate configurations (and
    // tests) never block each other.
    static std::string name_for_config(std::string_view config_path);

    // Location of the lock file: the per-user runtime directory on POSIX
    // ($XDG_RUNTIME_DIR, falling back to the temporary directory) and
    // %LOCALAPPDATA%\SwiftDM on Windows.
    static std::string lock_path(const std::string& name);

    // Endpoint for the "show the main window" request: an absolute socket path
    // on POSIX and a named pipe name on Windows.
    static std::string endpoint_path(const std::string& name);

    // Takes the lock for `role`. Returns false when another process holds it,
    // in which case owner() describes that process. A lock file left behind by
    // a dead run is taken over silently.
    bool acquire(InstanceRole role, const std::string& name, std::string* error = nullptr);
    void release();
    bool held() const { return held_; }
    InstanceRole role() const { return role_; }

    // Reads the lock file written by the current owner. Call it after a failed
    // acquire(); the file is written by the owner right after it takes the
    // lock, so a start that loses the race may see an empty file.
    InstanceOwner owner() const;

    // owner(), retried for `timeout_ms` while the record is still unreadable.
    // A refused start uses this so it can name the run in the way instead of
    // guessing during the short window between the owner's lock and its write.
    InstanceOwner owner_wait(int timeout_ms) const;

  private:
    bool write_owner() const;

    std::string path_;
    InstanceRole role_ = InstanceRole::Gui;
    bool held_ = false;
#ifdef _WIN32
    void* handle_ = nullptr;
#else
    int fd_ = -1;
#endif
};

}  // namespace swiftdm
