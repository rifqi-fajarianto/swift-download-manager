#pragma once

// Minimal FTP test server for CLI/backend integration tests (AGENTS.md: no GUI
// automation). Serves one deterministic file, supports SIZE/REST/EPSV and both
// passive modes, and counts REST commands so resume can be asserted.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "http_test_server.h"  // content_byte()

namespace testutil {

inline constexpr uint64_t kFtpFileSize = 256 * 1024;
inline std::atomic<int> g_ftp_rest_commands{0};
inline std::atomic<int> g_ftp_retr_commands{0};

class TestFtpServer {
  public:
    TestFtpServer() = default;
    ~TestFtpServer() { stop(); }

    TestFtpServer(const TestFtpServer&) = delete;
    TestFtpServer& operator=(const TestFtpServer&) = delete;

    bool start() {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (listen_fd_ < 0) return false;
        int yes = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            ::close(listen_fd_);
            listen_fd_ = -1;
            return false;
        }
        socklen_t len = sizeof(addr);
        if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
            ::close(listen_fd_);
            listen_fd_ = -1;
            return false;
        }
        port_ = ::ntohs(addr.sin_port);
        if (::listen(listen_fd_, 16) != 0) {
            ::close(listen_fd_);
            listen_fd_ = -1;
            return false;
        }
        running_.store(true);
        thread_ = std::thread([this] { accept_loop(); });
        return true;
    }

    void stop() {
        if (!running_.exchange(false)) return;
        if (listen_fd_ >= 0) {
            ::shutdown(listen_fd_, SHUT_RDWR);
            ::close(listen_fd_);
            listen_fd_ = -1;
        }
        if (thread_.joinable()) thread_.join();
    }

    std::string url(const std::string& path) const {
        return "ftp://127.0.0.1:" + std::to_string(port_) + path;
    }
    uint16_t port() const { return port_; }
    uint64_t file_size() const { return kFtpFileSize; }
    int rest_commands() const { return g_ftp_rest_commands.load(); }
    int retr_commands() const { return g_ftp_retr_commands.load(); }

  private:
    int listen_fd_ = -1;
    uint16_t port_ = 0;
    std::atomic<bool> running_{false};
    std::thread thread_;

    void accept_loop() {
        while (running_.load()) {
            sockaddr_in peer{};
            socklen_t len = sizeof(peer);
            const int fd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&peer), &len);
            if (fd < 0) break;
            std::thread([fd] { handle_conn(fd); }).detach();
        }
    }

    static bool send_all(int fd, const void* data, size_t len) {
        const char* p = static_cast<const char*>(data);
        while (len > 0) {
            const ssize_t n = ::send(fd, p, len, MSG_NOSIGNAL);
            if (n <= 0) return false;
            p += n;
            len -= static_cast<size_t>(n);
        }
        return true;
    }

    static bool send_line(int fd, const std::string& line) {
        const std::string text = line + "\r\n";
        return send_all(fd, text.data(), text.size());
    }

    static std::string upper(const std::string& text) {
        std::string out(text);
        for (char& c : out) {
            if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        }
        return out;
    }

    static int open_data_listener(uint16_t* port) {
        const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) return -1;
        int yes = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
            ::listen(fd, 4) != 0) {
            ::close(fd);
            return -1;
        }
        socklen_t len = sizeof(addr);
        if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
            ::close(fd);
            return -1;
        }
        *port = ::ntohs(addr.sin_port);
        return fd;
    }

    static int accept_data(int listener) {
        pollfd pfd{};
        pfd.fd = listener;
        pfd.events = POLLIN;
        if (::poll(&pfd, 1, 5000) <= 0) return -1;
        return ::accept(listener, nullptr, nullptr);
    }

    static void handle_conn(int fd) {
        std::string buffer;
        uint64_t rest_offset = 0;
        int data_listener = -1;
        uint16_t data_port = 0;
        bool quit = false;

        send_line(fd, "220 SwiftDM FTP test server ready");
        while (!quit) {
            size_t eol = std::string::npos;
            while ((eol = buffer.find("\r\n")) == std::string::npos) {
                char chunk[1024];
                const ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
                if (n <= 0) {
                    if (data_listener >= 0) ::close(data_listener);
                    ::close(fd);
                    return;
                }
                buffer.append(chunk, static_cast<size_t>(n));
                if (buffer.size() > 65536) {
                    if (data_listener >= 0) ::close(data_listener);
                    ::close(fd);
                    return;
                }
            }
            const std::string line = buffer.substr(0, eol);
            buffer.erase(0, eol + 2);
            const size_t space = line.find(' ');
            const std::string command = upper(line.substr(0, space));
            if (std::getenv("FTP_TEST_LOG")) std::fprintf(stderr, "FTP << %s\n", line.c_str());
            const std::string argument =
                space == std::string::npos ? std::string() : line.substr(space + 1);

            if (command == "USER") {
                send_line(fd, "331 Password required");
            } else if (command == "PASS") {
                send_line(fd, "230 Logged in");
            } else if (command == "TYPE" || command == "OPTS" || command == "MODE" ||
                       command == "STRU") {
                send_line(fd, "200 Ok");
            } else if (command == "PWD") {
                send_line(fd, "257 \"/\"");
            } else if (command == "CWD") {
                send_line(fd, "250 Directory changed");
            } else if (command == "FEAT") {
                const std::string features =
                    "211-Features:\r\n SIZE\r\n REST STREAM\r\n211 End\r\n";
                send_all(fd, features.data(), features.size());
            } else if (command == "SIZE") {
                send_line(fd, "213 " + std::to_string(kFtpFileSize));
            } else if (command == "MDTM") {
                send_line(fd, "213 20260101000000");
            } else if (command == "REST") {
                rest_offset = std::strtoull(argument.c_str(), nullptr, 10);
                // The NOBODY (SIZE) probe sends "REST 0"; only real resume
                // offsets are counted.
                if (rest_offset > 0) g_ftp_rest_commands.fetch_add(1);
                send_line(fd, "350 Restarting at " + argument);
            } else if (command == "EPSV" || command == "PASV") {
                if (data_listener >= 0) ::close(data_listener);
                data_listener = open_data_listener(&data_port);
                if (data_listener < 0) {
                    send_line(fd, "425 Cannot open data connection");
                } else if (command == "EPSV") {
                    send_line(fd, "229 Entering Extended Passive Mode (|||" +
                                      std::to_string(data_port) + "|)");
                } else {
                    const uint16_t p1 = static_cast<uint16_t>(data_port / 256);
                    const uint16_t p2 = static_cast<uint16_t>(data_port % 256);
                    send_line(fd, "227 Entering Passive Mode (127,0,0,1," +
                                      std::to_string(p1) + "," + std::to_string(p2) + ")");
                }
            } else if (command == "RETR") {
                if (data_listener < 0) {
                    send_line(fd, "425 Use PASV or EPSV first");
                    continue;
                }
                g_ftp_retr_commands.fetch_add(1);
                send_line(fd, "150 Opening data connection");
                const int data = accept_data(data_listener);
                if (data >= 0) {
                    std::vector<char> buf(8192);
                    uint64_t offset = rest_offset;
                    while (offset < kFtpFileSize) {
                        const uint64_t chunk =
                            std::min<uint64_t>(buf.size(), kFtpFileSize - offset);
                        for (uint64_t i = 0; i < chunk; ++i) {
                            buf[static_cast<size_t>(i)] =
                                static_cast<char>(content_byte(offset + i));
                        }
                        if (!send_all(data, buf.data(), static_cast<size_t>(chunk))) break;
                        offset += chunk;
                    }
                    ::close(data);
                }
                rest_offset = 0;
                ::close(data_listener);
                data_listener = -1;
                send_line(fd, "226 Transfer complete");
            } else if (command == "QUIT") {
                send_line(fd, "221 Bye");
                quit = true;
            } else {
                send_line(fd, "502 Command not implemented");
            }
        }
        if (data_listener >= 0) ::close(data_listener);
        ::close(fd);
    }
};

}  // namespace testutil
