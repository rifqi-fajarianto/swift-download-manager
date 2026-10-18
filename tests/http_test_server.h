#pragma once

// Minimal HTTP/1.1 test server for CLI/backend integration tests.
// Routes:
//   GET/HEAD /file/<size>[?slow=1]  ranges supported, deterministic content
//   GET/HEAD /chunked/<size>        chunked transfer, no ranges
//   GET/HEAD /redirect/<size>       302 to /file/<size>
//   GET/HEAD /forbidden             403 response
// Content bytes are shared with the tests via content_byte().

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace testutil {

inline uint8_t content_byte(uint64_t i) {
    uint32_t x = static_cast<uint32_t>(i) * 2654435761u + 12345u;
    x ^= x >> 13;
    x *= 2246822519u;
    x ^= x >> 16;
    return static_cast<uint8_t>(x & 0xff);
}

class TestHttpServer {
  public:
    TestHttpServer() = default;
    ~TestHttpServer() { stop(); }

    TestHttpServer(const TestHttpServer&) = delete;
    TestHttpServer& operator=(const TestHttpServer&) = delete;

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
        if (::listen(listen_fd_, 64) != 0) {
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

    std::string base_url() const {
        return "http://127.0.0.1:" + std::to_string(port_);
    }
    uint16_t port() const { return port_; }

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

    static bool send_str(int fd, const std::string& text) {
        return send_all(fd, text.data(), text.size());
    }

    static std::string common_headers(uint64_t total) {
        return "Content-Type: application/octet-stream\r\n"
               "Content-Disposition: attachment; filename=\"sample-" +
               std::to_string(total) + ".bin\"\r\n"
               "ETag: \"etag-" + std::to_string(total) + "\"\r\n"
               "Last-Modified: Wed, 21 Oct 2015 07:28:00 GMT\r\n"
               "Connection: close\r\n";
    }

    static void handle_conn(int fd) {
        std::string request;
        char buf[8192];
        while (request.find("\r\n\r\n") == std::string::npos && request.size() < 65536) {
            const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) {
                ::close(fd);
                return;
            }
            request.append(buf, static_cast<size_t>(n));
        }

        const size_t eol = request.find("\r\n");
        const std::string first = request.substr(0, eol == std::string::npos ? request.size() : eol);
        const size_t sp1 = first.find(' ');
        const size_t sp2 = first.rfind(' ');
        if (sp1 == std::string::npos || sp2 == std::string::npos || sp2 <= sp1) {
            ::close(fd);
            return;
        }
        const std::string method = first.substr(0, sp1);
        const std::string target = first.substr(sp1 + 1, sp2 - sp1 - 1);

        // Range header (case-insensitive lookup).
        std::string range;
        {
            const std::string lower = lower_copy(request);
            const size_t pos = lower.find("\r\nrange:");
            if (pos != std::string::npos) {
                const size_t begin = pos + 9;
                const size_t end = request.find("\r\n", begin);
                range = request.substr(begin, end == std::string::npos ? std::string::npos
                                                                      : end - begin);
                range = trim_copy(range);
            }
        }

        std::string path = target;
        std::string query;
        const size_t q = target.find('?');
        if (q != std::string::npos) {
            path = target.substr(0, q);
            query = target.substr(q + 1);
        }
        const bool slow = query.find("slow=1") != std::string::npos;

        if (path.rfind("/file/", 0) == 0) {
            serve_file(fd, method, path.substr(6), range, slow);
        } else if (path.rfind("/chunked/", 0) == 0) {
            serve_chunked(fd, method, path.substr(9));
        } else if (path.rfind("/redirect/", 0) == 0) {
            const std::string location = "/file/" + path.substr(10);
            send_str(fd, "HTTP/1.1 302 Found\r\nLocation: " + location +
                             "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
        } else if (path == "/forbidden") {
            const std::string body = "denied";
            send_str(fd, "HTTP/1.1 403 Forbidden\r\nContent-Length: " +
                             std::to_string(body.size()) +
                             "\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n" +
                             body);
        } else {
            const std::string body = "not found";
            send_str(fd, "HTTP/1.1 404 Not Found\r\nContent-Length: " +
                             std::to_string(body.size()) +
                             "\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n" +
                             body);
        }
        ::close(fd);
    }

    static std::string lower_copy(const std::string& s) {
        std::string out(s);
        for (char& c : out) {
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        }
        return out;
    }

    static std::string trim_copy(const std::string& s) {
        size_t b = 0, e = s.size();
        while (b < e && (s[b] == ' ' || s[b] == '\t')) ++b;
        while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r')) --e;
        return s.substr(b, e - b);
    }

    static bool parse_ranges(const std::string& header, uint64_t total, uint64_t& start,
                             uint64_t& end) {
        if (header.rfind("bytes=", 0) != 0) return false;
        const std::string spec = header.substr(6);
        const size_t dash = spec.find('-');
        if (dash == std::string::npos) return false;
        const std::string from = spec.substr(0, dash);
        const std::string to = spec.substr(dash + 1);
        if (from.empty()) return false;  // suffix ranges unsupported by the tests
        start = std::strtoull(from.c_str(), nullptr, 10);
        if (to.empty()) {
            end = total == 0 ? 0 : total - 1;
        } else {
            end = std::strtoull(to.c_str(), nullptr, 10);
        }
        if (total > 0 && end >= total) end = total - 1;
        if (start > end) return false;
        return true;
    }

    static void serve_file(int fd, const std::string& method, const std::string& size_text,
                           const std::string& range_header, bool slow) {
        const uint64_t total = std::strtoull(size_text.c_str(), nullptr, 10);
        uint64_t start = 0;
        uint64_t end = total == 0 ? 0 : total - 1;
        bool partial = false;
        if (method != "HEAD" && !range_header.empty() && total > 0) {
            partial = parse_ranges(range_header, total, start, end);
        }

        std::string head;
        if (partial) {
            head = "HTTP/1.1 206 Partial Content\r\nContent-Length: " +
                   std::to_string(end - start + 1) +
                   "\r\nContent-Range: bytes " + std::to_string(start) + "-" +
                   std::to_string(end) + "/" + std::to_string(total) +
                   "\r\nAccept-Ranges: bytes\r\n" + common_headers(total) + "\r\n";
        } else {
            head = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(total) +
                   "\r\nAccept-Ranges: bytes\r\n" + common_headers(total) + "\r\n";
        }
        if (!send_str(fd, head)) return;
        if (method == "HEAD" || total == 0) return;

        std::vector<char> buf(65536);
        uint64_t offset = start;
        while (offset <= end) {
            uint64_t chunk = end - offset + 1;
            if (chunk > buf.size()) chunk = buf.size();
            for (uint64_t i = 0; i < chunk; ++i) {
                buf[static_cast<size_t>(i)] = static_cast<char>(content_byte(offset + i));
            }
            if (!send_all(fd, buf.data(), static_cast<size_t>(chunk))) return;
            offset += chunk;
            if (slow) ::usleep(60000);  // ~1 MB/s per connection
        }
    }

    static void serve_chunked(int fd, const std::string& method,
                              const std::string& size_text) {
        const uint64_t total = std::strtoull(size_text.c_str(), nullptr, 10);
        std::string head = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n" +
                           common_headers(total) + "\r\n";
        if (!send_str(fd, head)) return;
        if (method == "HEAD" || total == 0) return;

        std::vector<char> buf(4096);
        uint64_t offset = 0;
        while (offset < total) {
            uint64_t chunk = total - offset;
            if (chunk > buf.size()) chunk = buf.size();
            for (uint64_t i = 0; i < chunk; ++i) {
                buf[static_cast<size_t>(i)] = static_cast<char>(content_byte(offset + i));
            }
            char header[32];
            std::snprintf(header, sizeof(header), "%llx\r\n",
                          static_cast<unsigned long long>(chunk));
            if (!send_str(fd, header)) return;
            if (!send_all(fd, buf.data(), static_cast<size_t>(chunk))) return;
            if (!send_str(fd, "\r\n")) return;
            offset += chunk;
        }
        send_str(fd, "0\r\n\r\n");
    }
};

}  // namespace testutil
