#include "core/util.h"

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <utility>

#include "core/win_headers.h"

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace swiftdm {
namespace {

#ifdef _WIN32
// Reads an environment variable as UTF-16 and returns it as UTF-8. The C runtime
// getenv() would mangle paths for a user name outside the ANSI code page.
std::string env_utf8(const wchar_t* name) {
    const wchar_t* value = _wgetenv(name);
    if (!value || !*value) return {};
    const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1,
                                          nullptr, 0, nullptr, nullptr);
    if (bytes <= 1) return {};
    std::string result(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, result.data(),
                        bytes, nullptr, nullptr);
    result.pop_back();  // Drop the terminator included in the conversion.
    return result;
}
#endif

}  // namespace

std::string home_dir() {
#ifdef _WIN32
    return env_utf8(L"USERPROFILE");
#else
    const char* h = std::getenv("HOME");
    return h ? h : "";
#endif
}

std::string expand_tilde(std::string_view path) {
    if (!path.empty() && path[0] == '~') {
        std::string home = home_dir();
        if (path.size() == 1) return home;
        if (path[1] == '/' || path[1] == '\\') return home + "/" + std::string(path.substr(2));
    }
    return std::string(path);
}

std::string xdg_config_home() {
#ifdef _WIN32
    const std::string roaming = env_utf8(L"APPDATA");
    return roaming.empty() ? home_dir() + "/AppData/Roaming" : roaming;
#else
    const char* c = std::getenv("XDG_CONFIG_HOME");
    if (c && *c) return c;
    return home_dir() + "/.config";
#endif
}

std::string xdg_data_home() {
#ifdef _WIN32
    const std::string local = env_utf8(L"LOCALAPPDATA");
    return local.empty() ? home_dir() + "/AppData/Local" : local;
#else
    const char* c = std::getenv("XDG_DATA_HOME");
    if (c && *c) return c;
    return home_dir() + "/.local/share";
#endif
}

#ifdef _WIN32
namespace {

// Win32 rejects paths longer than MAX_PATH unless they carry the extended-length
// prefix, which also disables "/" handling, so the separators are normalized
// first. Names of path components already fit the limit; it is the full path
// (temp directory + long file name) that can exceed it.
std::wstring extended_length_path(const std::wstring& input) {
    if (input.size() < 248) return input;
    if (input.rfind(L"\\\\?\\", 0) == 0) return input;
    std::wstring normalized = input;
    for (wchar_t& c : normalized) {
        if (c == L'/') c = L'\\';
    }
    if (normalized.rfind(L"\\\\", 0) == 0) {
        return L"\\\\?\\UNC\\" + normalized.substr(2);
    }
    if (normalized.size() > 1 && normalized[1] == L':') {
        return L"\\\\?\\" + normalized;
    }
    // A relative path would change meaning, so it stays as it is.
    return input;
}

std::wstring plain_path(const std::wstring& input) {
    if (input.rfind(L"\\\\?\\UNC\\", 0) == 0) return L"\\\\" + input.substr(8);
    if (input.rfind(L"\\\\?\\", 0) == 0) return input.substr(4);
    return input;
}

// Windows paths are UTF-16. Converting explicitly (without MB_ERR_INVALID_CHARS)
// keeps valid UTF-8 exact and turns stray bytes from an old database or a legacy
// console code page into U+FFFD instead of throwing filesystem_error.
std::wstring utf8_to_wide(std::string_view text) {
    if (text.empty()) return {};
    const int needed = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                           static_cast<int>(text.size()), nullptr, 0);
    if (needed <= 0) return {};
    std::wstring wide(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        wide.data(), needed);
    return wide;
}

}  // namespace
#endif

std::filesystem::path path_from_utf8(std::string_view path) {
#ifdef _WIN32
    return std::filesystem::path(extended_length_path(utf8_to_wide(path)));
#else
    // C++20 spells the UTF-8 entry point as the char8_t constructor; u8path is
    // deprecated and warns on newer toolchains.
    const auto* first = reinterpret_cast<const char8_t*>(path.data());
    return std::filesystem::path(std::u8string(first, first + path.size()));
#endif
}

std::string path_to_utf8(const std::filesystem::path& path) {
#ifdef _WIN32
    // The prefix is an implementation detail of the OS calls; it must not leak
    // into the history database or the UI. Converting with Win32 also keeps the
    // result valid UTF-8 even for a name the filesystem stored as unpaired
    // surrogates, which the standard conversion would reject by throwing.
    const std::wstring value = plain_path(path.native());
    if (value.empty()) return {};
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, value.data(),
                                          static_cast<int>(value.size()), nullptr, 0,
                                          nullptr, nullptr);
    if (bytes <= 0) return {};
    std::string result(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                        result.data(), bytes, nullptr, nullptr);
    return result;
#else
    const auto value = path.u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
#endif
}

bool ensure_directory(const std::string& path, std::string* error) {
    std::error_code ec;
    std::filesystem::create_directories(path_from_utf8(path), ec);
    if (ec) {
        if (error) *error = "cannot create directory '" + path + "': " + ec.message();
        return false;
    }
    return true;
}

std::string executable_dir() {
#ifdef _WIN32
    // GetModuleFileNameW grows the buffer instead of failing outright, so the
    // loop only repeats when the path was truncated.
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD len = GetModuleFileNameW(nullptr, buffer.data(),
                                             static_cast<DWORD>(buffer.size()));
        if (len == 0) return {};
        if (len < buffer.size()) {
            buffer.resize(len);
            break;
        }
        buffer.resize(buffer.size() * 2);
    }
    return path_to_utf8(std::filesystem::path(buffer).parent_path());
#else
    std::error_code ec;
    const auto exe = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (ec) return {};
    return path_to_utf8(exe.parent_path());
#endif
}

std::string resolve_ca_bundle(const std::string& exe_dir, std::string_view env_override) {
    // A path that names no file is dropped instead of returned: libcurl would
    // report CURLE_SSL_CACERT_BADFILE for it, which is exactly the failure
    // this lookup exists to remove.
    const auto usable = [](std::string_view path) {
        if (path.empty()) return false;
        std::error_code ec;
        return std::filesystem::is_regular_file(path_from_utf8(path), ec) && !ec;
    };
    if (usable(env_override)) return std::string(env_override);
    if (!exe_dir.empty()) {
        const std::string candidate =
            path_to_utf8(path_from_utf8(exe_dir) / "ca-bundle.crt");
        if (usable(candidate)) return candidate;
    }
    return {};
}

std::string resolve_ca_bundle() {
#ifdef _WIN32
    return resolve_ca_bundle(executable_dir(), env_utf8(L"SWIFTDM_CA_BUNDLE"));
#else
    const char* env = std::getenv("SWIFTDM_CA_BUNDLE");
    return resolve_ca_bundle(executable_dir(), env ? std::string_view(env) : std::string_view());
#endif
}

#ifdef _WIN32
namespace {

// std::filesystem::remove_all uses a recursive iterator, which cannot advance
// past a directory entry whose full path exceeds MAX_PATH and then spins
// forever. Walking the tree with Win32 calls and an extended-length path avoids
// the iterator completely.
bool remove_tree_native(const std::wstring& dir) {
    WIN32_FIND_DATAW data{};
    const std::wstring pattern = dir + L"\\*";
    const HANDLE find = FindFirstFileW(pattern.c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) {
        const DWORD code = GetLastError();
        return code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND;
    }
    bool ok = true;
    do {
        const std::wstring name = data.cFileName;
        if (name == L"." || name == L"..") continue;
        const std::wstring child = dir + L"\\" + name;
        const DWORD attributes = data.dwFileAttributes;
        if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
                // A junction or directory symlink is unlinked, never followed.
                if (!RemoveDirectoryW(child.c_str())) ok = false;
            } else if (!remove_tree_native(child)) {
                ok = false;
            }
            continue;
        }
        if (!DeleteFileW(child.c_str())) {
            const bool read_only = (attributes & FILE_ATTRIBUTE_READONLY) != 0;
            if (!read_only || !SetFileAttributesW(child.c_str(), FILE_ATTRIBUTE_NORMAL) ||
                !DeleteFileW(child.c_str())) {
                ok = false;
            }
        }
    } while (FindNextFileW(find, &data));
    FindClose(find);
    if (!RemoveDirectoryW(dir.c_str())) ok = false;
    return ok;
}

}  // namespace
#endif

bool remove_tree(const std::string& path) {
    const std::filesystem::path target = path_from_utf8(path);
    std::error_code ec;
    if (!std::filesystem::is_directory(target, ec)) {
        return !std::filesystem::exists(target, ec) || std::filesystem::remove(target, ec);
    }
#ifdef _WIN32
    return remove_tree_native(target.native());
#else
    std::filesystem::remove_all(target, ec);
    return !ec;
#endif
}

bool ensure_private_directory(const std::string& path, std::string* error) {
    if (!ensure_directory(path, error)) return false;
#ifdef _WIN32
    // Windows inherits the directory ACL from the parent (the user profile),
    // so there is no POSIX mode bit to tighten here.
    if (error) error->clear();
    return true;
#else
    const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        if (error) *error = "cannot open private directory '" + path + "': " +
                            std::strerror(errno);
        return false;
    }
    if (::fchmod(fd, 0700) != 0) {
        if (error) *error = "cannot set private permissions on '" + path +
                            "': " + std::strerror(errno);
        ::close(fd);
        return false;
    }
    ::close(fd);
    if (error) error->clear();
    return true;
#endif
}

bool write_file_atomic(const std::string& path, std::string_view content,
                       unsigned int mode, std::string* error) {
#ifdef _WIN32
    // No mkstemp/fchmod on Windows: write a sibling temporary file through the
    // wide-path layer and swap it in with a replace move, which is atomic on a
    // single volume.
    (void)mode;  // the file inherits the ACL of its directory
    const std::filesystem::path target = path_from_utf8(path);
    const std::filesystem::path parent =
        target.has_parent_path() ? target.parent_path() : path_from_utf8(".");
    const std::filesystem::path temporary =
        parent / path_from_utf8(path_to_utf8(target.filename()) + ".tmp");
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) {
            if (error) *error = "cannot create temporary file for '" + path + "'";
            return false;
        }
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        out.flush();
        if (!out) {
            if (error) *error = "cannot write temporary file for '" + path + "'";
            out.close();
            std::error_code ec;
            std::filesystem::remove(temporary, ec);
            return false;
        }
    }
    if (!MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        if (error) *error = "cannot replace '" + path + "' (error " +
                            std::to_string(GetLastError()) + ")";
        std::error_code ec;
        std::filesystem::remove(temporary, ec);
        return false;
    }
    if (error) error->clear();
    return true;
#else
    const std::filesystem::path target(path);
    const std::filesystem::path parent = target.has_parent_path() ? target.parent_path() : ".";
    std::string temporary = (parent / (target.filename().string() + ".tmp-XXXXXX")).string();
    std::vector<char> temporary_buffer(temporary.begin(), temporary.end());
    temporary_buffer.push_back('\0');
    const int fd = ::mkstemp(temporary_buffer.data());
    if (fd < 0) {
        if (error) *error = "cannot create temporary file for '" + path + "': " +
                            std::strerror(errno);
        return false;
    }
    temporary.assign(temporary_buffer.data());
    if (::fcntl(fd, F_SETFD, FD_CLOEXEC) != 0) {
        if (error) *error = "cannot secure temporary file for '" + path + "': " +
                            std::strerror(errno);
        ::close(fd);
        ::unlink(temporary.c_str());
        return false;
    }

    bool ok = true;
    size_t offset = 0;
    while (offset < content.size()) {
        const ssize_t written = ::write(fd, content.data() + offset, content.size() - offset);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) {
            if (error) *error = "cannot write temporary file for '" + path + "': " +
                                std::strerror(errno);
            ok = false;
            break;
        }
        offset += static_cast<size_t>(written);
    }
    if (ok && ::fchmod(fd, static_cast<mode_t>(mode)) != 0) {
        if (error) *error = "cannot set file permissions for '" + path + "': " +
                            std::strerror(errno);
        ok = false;
    }
    if (ok && ::fsync(fd) != 0) {
        if (error) *error = "cannot sync temporary file for '" + path + "': " +
                            std::strerror(errno);
        ok = false;
    }
    if (::close(fd) != 0 && ok) {
        if (error) *error = "cannot close temporary file for '" + path + "': " +
                            std::strerror(errno);
        ok = false;
    }
    if (ok && ::rename(temporary.c_str(), path.c_str()) != 0) {
        if (error) *error = "cannot replace '" + path + "': " + std::strerror(errno);
        ok = false;
    }
    if (ok) {
        const int dir_fd = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (dir_fd >= 0) {
            // The rename has already committed. Directory fsync is best-effort
            // on filesystems that do not support syncing directory handles.
            (void)::fsync(dir_fd);
            ::close(dir_fd);
        }
        if (error) error->clear();
    } else {
        ::unlink(temporary.c_str());
    }
    return ok;
#endif
}

namespace {

int64_t file_time_to_unix(std::filesystem::file_time_type time) {
    const auto system_time = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        time - std::filesystem::file_time_type::clock::now() +
        std::chrono::system_clock::now());
    return static_cast<int64_t>(std::chrono::system_clock::to_time_t(system_time));
}

bool is_unique_suffix(const std::string& name, const std::string& stem,
                      const std::string& ext) {
    if (!starts_with(name, stem + " (")) return false;
    const size_t open = stem.size() + 2;
    const size_t close = name.find(')', open);
    if (close == std::string::npos) return false;
    if (close == open) return false;
    for (size_t i = open; i < close; ++i) {
        if (name[i] < '0' || name[i] > '9') return false;
    }
    return name.substr(close + 1) == ext;
}

}  // namespace

std::string resolve_existing_file(const std::string& dir, const std::string& filename,
                                  int64_t near_unix) {
    if (dir.empty() || filename.empty()) return {};
    std::error_code ec;
    const std::filesystem::path exact = path_from_utf8(dir) / path_from_utf8(filename);
    if (std::filesystem::is_regular_file(exact, ec)) return path_to_utf8(exact);

    const std::filesystem::path base = path_from_utf8(filename);
    const std::string stem = path_to_utf8(base.stem());
    const std::string ext = path_to_utf8(base.extension());
    if (stem.empty()) return {};

    std::string best;
    int64_t best_distance = 0;
    for (std::filesystem::directory_iterator it(path_from_utf8(dir), ec), end; !ec && it != end;
         it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const std::string name = path_to_utf8(it->path().filename());
        if (!is_unique_suffix(name, stem, ext)) continue;
        const int64_t written = file_time_to_unix(
            std::filesystem::last_write_time(it->path(), ec));
        if (ec) continue;
        const int64_t distance =
            near_unix > 0 ? std::llabs(written - near_unix) : 0;
        if (best.empty() || distance < best_distance) {
            best = path_to_utf8(it->path());
            best_distance = distance;
        }
    }
    return best;
}

}  // namespace swiftdm
