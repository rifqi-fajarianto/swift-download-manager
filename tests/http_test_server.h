#pragma once

// Minimal HTTP/1.1 test server for CLI/backend integration tests.
// Routes:
//   GET/HEAD /file/<size>[?slow=1][&slowfrom=<offset>&slowms=<ms>]
//                                   ranges supported, deterministic content;
//                                   slowfrom throttles only the range that
//                                   starts at that offset
//   GET/HEAD /if-range-changed/<size> returns a new ETag for If-Range tests
//   GET/HEAD /space-file/<name>     fixed file; exercises escaped path spaces
//   GET/HEAD /chunked/<size>        chunked transfer, no ranges
//   GET/HEAD /redirect/<size>       302 to /file/<size>
//   GET/HEAD /redirect-cross-origin/<size> 302 changes 127.0.0.1 to localhost
//   GET/HEAD /forbidden             403 response
//   GET/HEAD /json-error            403 with a JSON error body (pixeldrain shape)
//   GET/HEAD /segments-forbidden/N  200 probe, ranged GETs answer 403 JSON
//   GET/HEAD /stream-forbidden      200 chunked probe, streamed GET answers 403 JSON
//   GET/HEAD /limit-connections/N   200 probe, parallel ranges answer 403 JSON
//   GET/HEAD /limit-connections-503/N?max=M  200 probe, ranges beyond M
//                                   answer 503 with an UploadHaven-shaped page
//   GET/HEAD /retry-after-503/N     200 probe, first ranged GET answers 503
//                                   with Retry-After: 1
//   GET/HEAD /drive-page/N          Google Drive virus-scan notice; with
//                                   confirm=t the same route serves /file/N
//   GET/HEAD /drive-loop/N          virus-scan notice that never resolves
//   GET/HEAD /signin                Google sign-in page (private-file wall)
//   GET/HEAD /page                  plain HTML page (no file download)
//   GET/HEAD /confirm-form-page     HTML page whose Drive-shaped form points at
//                                   another origin (must not be followed)
//   GET/HEAD /confirm-target        counts requests; the off-origin form target
//   GET/HEAD /html-error            403 HTML page with a <title>
//   GET/HEAD /html-error-plain      403 HTML page without a <title>
//   GET/HEAD /hls/master.m3u8       HLS master playlist (variant-1/-2)
//   GET/HEAD /hls/variant-<n>.m3u8  HLS media playlist (3 segments)
//   GET/HEAD /hls/seg-<v>-<s>.ts    HLS segment (deterministic content)
//   GET/HEAD /hls/encrypted.m3u8    playlist with EXT-X-KEY (unsupported)
//   GET/HEAD /hls/map.m3u8          playlist with EXT-X-MAP init segment
//   GET/HEAD /hls/file-segment.m3u8 playlist whose segment is file:///etc/passwd
//   GET/HEAD /hls/file-variant.m3u8 master playlist with a file:// variant
//   GET/HEAD /hls/file-map.m3u8     EXT-X-MAP pointing at file:///etc/passwd
//   GET/HEAD /dash/manifest.mpd     DASH MPD (SegmentTemplate + SegmentTimeline)
//   GET/HEAD /dash/init-<rep>.mp4   DASH initialisation segment
//   GET/HEAD /dash/seg-<rep>-<n>.m4s DASH media segment
//   GET/HEAD /mse/seg-<n>.m4s       MSE media segment (mfhd sequence n)
//   GET/HEAD /mse/gap-<n>.m4s       MSE segment with sequence 2n (gap)
// Content bytes are shared with the tests via content_byte().

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
// Winsock calls take SOCKET, which is wider than int on 64-bit Windows.
using socklen_t = int;
using socket_t = SOCKET;
inline constexpr socket_t kInvalidSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
inline constexpr socket_t kInvalidSocket = -1;
#endif

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include <climits>
#include <type_traits>

namespace testutil {

inline int close_socket(socket_t socket) {
#ifdef _WIN32
    return ::closesocket(socket);
#else
    return ::close(socket);
#endif
}
inline bool socket_valid(socket_t socket) { return socket != kInvalidSocket; }

// Portable millisecond sleep for the throttled test routes.
inline void sleep_ms(int ms) {
#ifdef _WIN32
    ::Sleep(static_cast<DWORD>(ms));
#else
    ::usleep(static_cast<useconds_t>(ms) * 1000);
#endif
}

inline std::atomic<int> g_cookie_requests{0};
inline std::atomic<int> g_non_matching_cookie_requests{0};
inline std::atomic<int> g_head_requests{0};
inline std::atomic<int> g_limited_active{0};
inline std::atomic<int> g_limited_refusals{0};
inline std::atomic<int> g_limited_503_active{0};
inline std::atomic<int> g_limited_503_peak{0};
inline std::atomic<int> g_limited_503_refusals{0};
inline std::atomic<int> g_retry_after_refusals{0};
inline std::atomic<int> g_confirm_form_requests{0};
inline std::atomic<int> g_cross_origin_redirect_targets{0};
inline std::atomic<int> g_forwarded_sensitive_headers{0};
inline std::atomic<int> g_forwarded_referers{0};
inline std::atomic<int> g_cross_origin_redirects{0};
inline std::atomic<int> g_if_range_requests{0};
inline std::atomic<int> g_invalid_content_range_mode{0};

inline uint8_t content_byte(uint64_t i) {
    uint32_t x = static_cast<uint32_t>(i) * 2654435761u + 12345u;
    x ^= x >> 13;
    x *= 2246822519u;
    x ^= x >> 16;
    return static_cast<uint8_t>(x & 0xff);
}

// HLS fixtures: every segment is 16 KiB and distinguishable by variant/index.
inline constexpr uint64_t kHlsSegmentSize = 16 * 1024;

// `slowfrom` sentinel: throttle nothing unless a test asks for a specific range.
inline constexpr uint64_t kNoSlowFrom = ~static_cast<uint64_t>(0);

inline uint8_t hls_segment_byte(int variant, int segment, uint64_t i) {
    return content_byte(static_cast<uint64_t>(variant) * 1000000u +
                        static_cast<uint64_t>(segment) * 100000u + i);
}

class TestHttpServer {
  public:
    TestHttpServer() = default;
    ~TestHttpServer() { stop(); }

    TestHttpServer(const TestHttpServer&) = delete;
    TestHttpServer& operator=(const TestHttpServer&) = delete;

    bool start() {
#ifdef _WIN32
        static const bool winsock_ready = [] {
            WSADATA data{};
            return ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
        }();
        if (!winsock_ready) return false;
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#else
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
#endif
        if (!socket_valid(listen_fd_)) return false;
        int yes = 1;
#ifdef _WIN32
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR,
                     reinterpret_cast<const char*>(&yes), sizeof(yes));
#else
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
#endif
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            close_socket(listen_fd_);
            listen_fd_ = kInvalidSocket;
            return false;
        }
        socklen_t len = sizeof(addr);
        if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
            close_socket(listen_fd_);
            listen_fd_ = kInvalidSocket;
            return false;
        }
        port_ = ::ntohs(addr.sin_port);
        if (::listen(listen_fd_, 64) != 0) {
            close_socket(listen_fd_);
            listen_fd_ = kInvalidSocket;
            return false;
        }
        running_.store(true);
        // The descriptor is passed by value: stop() closes it while the accept
        // loop may still be inside accept(), so a shared member would race.
        thread_ = std::thread([this, fd = listen_fd_, port = port_] { accept_loop(fd, port); });
        return true;
    }

    void stop() {
        if (!running_.exchange(false)) return;
        if (socket_valid(listen_fd_)) {
#ifdef _WIN32
            ::shutdown(listen_fd_, SD_BOTH);
#else
            ::shutdown(listen_fd_, SHUT_RDWR);
#endif
            close_socket(listen_fd_);
            listen_fd_ = kInvalidSocket;
        }
        if (thread_.joinable()) thread_.join();
    }

    std::string base_url() const {
        return "http://127.0.0.1:" + std::to_string(port_);
    }
    uint16_t port() const { return port_; }

  private:
    socket_t listen_fd_ = kInvalidSocket;
    uint16_t port_ = 0;
    std::atomic<bool> running_{false};
    std::thread thread_;

  public:
    int cookie_requests() const { return g_cookie_requests.load(); }
    int non_matching_cookie_requests() const { return g_non_matching_cookie_requests.load(); }
    int limited_refusals() const { return g_limited_refusals.load(); }
    int limited_503_refusals() const { return g_limited_503_refusals.load(); }
    int limited_503_peak() const { return g_limited_503_peak.load(); }
    int head_requests() const { return g_head_requests.load(); }
    void reset_limited_refusals() { g_limited_refusals.store(0); }
    void reset_limited_503() {
        g_limited_503_active.store(0);
        g_limited_503_peak.store(0);
        g_limited_503_refusals.store(0);
        g_retry_after_refusals.store(0);
    }

  private:
    void accept_loop(int listen_fd, uint16_t port) {
        while (running_.load()) {
            sockaddr_in peer{};
            socklen_t len = sizeof(peer);
            const socket_t fd = ::accept(listen_fd, reinterpret_cast<sockaddr*>(&peer), &len);
            if (!socket_valid(fd)) break;
            std::thread([this, fd, port] { handle_conn(fd, port); }).detach();
        }
    }

    static bool send_all(
        socket_t fd,
        const void* data, size_t len) {
        const char* p = static_cast<const char*>(data);
        while (len > 0) {
#ifdef _WIN32
            const int n = ::send(fd, p, static_cast<int>(std::min<size_t>(len, INT_MAX)), 0);
#else
            const ssize_t n = ::send(fd, p, len, MSG_NOSIGNAL);
#endif
            if (n <= 0) return false;
            p += n;
            len -= static_cast<size_t>(n);
        }
        return true;
    }

    static bool send_str(
        socket_t fd,
        const std::string& text) {
        return send_all(fd, text.data(), text.size());
    }

    static std::string common_headers(uint64_t total, const std::string& if_range = {},
                                      bool force_changed = false) {
        const std::string etag = "\"etag-" + std::to_string(total) + "\"";
        const bool mismatch = force_changed ||
                              (!if_range.empty() && if_range != etag &&
                               if_range != "Wed, 21 Oct 2015 07:28:00 GMT");
        const std::string current_etag =
            mismatch ? "\"etag-" + std::to_string(total + 1) + "\"" : etag;
        const std::string etag_line =
            g_invalid_content_range_mode.load() == 4
                ? std::string()
                : "ETag: " + current_etag + "\r\n";
        const std::string last_modified_line =
            g_invalid_content_range_mode.load() == 4
                ? std::string()
                : "Last-Modified: Wed, 21 Oct 2015 07:28:00 GMT\r\n";
        return "Content-Type: application/octet-stream\r\n"
               "Content-Disposition: attachment; filename=\"sample-" +
               std::to_string(total) + ".bin\"\r\n" + etag_line + last_modified_line +
               "Connection: close\r\n";
    }

    static void handle_conn(
        socket_t fd,
        uint16_t port) {
        std::string request;
        char buf[8192];
        while (request.find("\r\n\r\n") == std::string::npos && request.size() < 65536) {
#ifdef _WIN32
            const int n = ::recv(fd, buf, sizeof(buf), 0);
#else
            const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
#endif
            if (n <= 0) {
                close_socket(fd);
                return;
            }
            request.append(buf, static_cast<size_t>(n));
        }

        const size_t eol = request.find("\r\n");
        const std::string first = request.substr(0, eol == std::string::npos ? request.size() : eol);
        const size_t sp1 = first.find(' ');
        const size_t sp2 = first.rfind(' ');
        if (sp1 == std::string::npos || sp2 == std::string::npos || sp2 <= sp1) {
            close_socket(fd);
            return;
        }
        const std::string method = first.substr(0, sp1);
        const std::string target = first.substr(sp1 + 1, sp2 - sp1 - 1);
        if (method == "HEAD") g_head_requests.fetch_add(1);

        // Range header (case-insensitive lookup).
        std::string range;
        std::string if_range;
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
            const size_t validator_pos = lower.find("\r\nif-range:");
            if (validator_pos != std::string::npos) {
                const size_t begin = validator_pos + 11;
                const size_t end = request.find("\r\n", begin);
                if_range = trim_copy(request.substr(
                    begin, end == std::string::npos ? std::string::npos : end - begin));
                g_if_range_requests.fetch_add(1);
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
        // Throttle only the request whose range starts exactly at `slowfrom`, so
        // a test can make one segment (the straggler) slow while a split thief
        // that starts elsewhere still runs at full speed.
        uint64_t slow_from = kNoSlowFrom;
        int slow_ms = 60;
        {
            const size_t from_pos = query.find("slowfrom=");
            if (from_pos != std::string::npos) {
                slow_from = std::strtoull(query.c_str() + from_pos + 9, nullptr, 10);
            }
            const size_t ms_pos = query.find("slowms=");
            if (ms_pos != std::string::npos) {
                const int value = std::atoi(query.c_str() + ms_pos + 7);
                if (value > 0) slow_ms = value;
            }
        }

        const std::string lower_request = lower_copy(request);
        const size_t cookie_pos = lower_request.find("\r\ncookie:");
        if (cookie_pos != std::string::npos) {
            const size_t begin = cookie_pos + 9;
            const size_t end = request.find("\r\n", begin);
            const std::string cookie = request.substr(
                begin, end == std::string::npos ? std::string::npos : end - begin);
            if (cookie.find("swift_test=secret") != std::string::npos) {
                g_cookie_requests.fetch_add(1);
            }
            if (cookie.find("other_test=private") != std::string::npos) {
                g_non_matching_cookie_requests.fetch_add(1);
            }
        }

        const size_t sensitive_header = lower_request.find("\r\nx-audit-token:");
        if (sensitive_header != std::string::npos && path.rfind("/file/", 0) == 0) {
            g_forwarded_sensitive_headers.fetch_add(1);
        }
        const size_t referer_header = lower_request.find("\r\nreferer:");
        if (referer_header != std::string::npos && path.rfind("/file/", 0) == 0) {
            g_forwarded_referers.fetch_add(1);
        }

        if (path.rfind("/file/", 0) == 0) {
            if (lower_request.find("\r\nhost: localhost:") != std::string::npos) {
                g_cross_origin_redirect_targets.fetch_add(1);
            }
            serve_file(fd, method, path.substr(6), range, slow, if_range, false, slow_from,
                       slow_ms);
        } else if (path.rfind("/if-range-changed/", 0) == 0) {
            serve_file(fd, method, path.substr(18), range, slow, if_range,
                       method != "HEAD");
        } else if (path.rfind("/space-file/", 0) == 0) {
            serve_file(fd, method, "65536", range, false);
        } else if (path.rfind("/chunked/", 0) == 0) {
            serve_chunked(fd, method, path.substr(9));
        } else if (path.rfind("/redirect/", 0) == 0) {
            const std::string location = "/file/" + path.substr(10);
            send_str(fd, "HTTP/1.1 302 Found\r\nLocation: " + location +
                             "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
        } else if (path.rfind("/redirect-cross-origin/", 0) == 0) {
            g_cross_origin_redirects.fetch_add(1);
            const std::string location = "http://localhost:" + std::to_string(port) +
                                         "/file/" + path.substr(23);
            send_str(fd, "HTTP/1.1 302 Found\r\nLocation: " + location +
                             "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
        } else if (path.rfind("/hls/", 0) == 0) {
            serve_hls(fd, method, path);
        } else if (path.rfind("/dash/", 0) == 0) {
            serve_dash(fd, method, path);
        } else if (path.rfind("/mse/", 0) == 0) {
            serve_mse(fd, method, path);
        } else if (path == "/forbidden") {
            const std::string body = "denied";
            send_str(fd, "HTTP/1.1 403 Forbidden\r\nContent-Length: " +
                             std::to_string(body.size()) +
                             "\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n" +
                             body);
        } else if (path == "/html-error") {
            const std::string body =
                "<html><head><title>Sorry, you have been blocked</title>"
                "<style>body{color:red}</style><script>var a = 1;</script></head>"
                "<body><h1>Access denied</h1></body></html>";
            send_str(fd, "HTTP/1.1 403 Forbidden\r\nContent-Length: " +
                             std::to_string(body.size()) +
                             "\r\nContent-Type: text/html; charset=UTF-8\r\nConnection: "
                             "close\r\n\r\n" +
                             body);
        } else if (path == "/html-error-plain") {
            const std::string body =
                "<html><body><script>var a = 1;</script><style>b{}</style>"
                "Request blocked by the host</body></html>";
            send_str(fd, "HTTP/1.1 403 Forbidden\r\nContent-Length: " +
                             std::to_string(body.size()) +
                             "\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n" +
                             body);
        } else if (path == "/json-error") {
            // Body shape used by pixeldrain and other JSON back ends: the reason
            // lives in "value"/"message" and must reach the failure message.
            send_json_error(fd, "403 Forbidden", hotlink_error_body());
        } else if (path.rfind("/segments-forbidden/", 0) == 0) {
            // The probe (HEAD) succeeds and advertises ranges, while every
            // ranged GET is refused like a host that limits concurrent
            // connections.
            if (method == "HEAD" || range.empty()) {
                const uint64_t total = std::strtoull(
                    path.substr(std::string("/segments-forbidden/").size()).c_str(), nullptr, 10);
                std::string head = "HTTP/1.1 200 OK\r\nContent-Length: " +
                                   std::to_string(total) +
                                   "\r\nAccept-Ranges: bytes\r\n" + common_headers(total) + "\r\n";
                send_str(fd, head);
            } else {
                send_json_error(fd, "403 Forbidden", connection_limit_error_body());
            }
        } else if (path.rfind("/limit-connections/", 0) == 0) {
            serve_limited_connections(fd, method, range,
                                      path.substr(std::string("/limit-connections/").size()));
        } else if (path.rfind("/limit-connections-503/", 0) == 0) {
            // <size>?max=<n>: like UploadHaven, only `max` parallel ranges run.
            const int max_active = query_int(query, "max", 2);
            serve_limited_connections_503(
                fd, method, range, path.substr(std::string("/limit-connections-503/").size()),
                max_active < 1 ? 1 : max_active);
        } else if (path.rfind("/retry-after-503/", 0) == 0) {
            serve_retry_after_503(fd, method, range,
                                  path.substr(std::string("/retry-after-503/").size()));
        } else if (path.rfind("/drive-page/", 0) == 0 || path.rfind("/drive-loop/", 0) == 0) {
            serve_drive_page(fd, method, port, path, query, range);
        } else if (path == "/signin") {
            serve_signin_page(fd);
        } else if (path == "/page") {
            serve_plain_page(fd);
        } else if (path == "/confirm-form-page") {
            serve_confirm_form_page(fd, port);
        } else if (path == "/confirm-target") {
            // If a probe follows the off-origin form, this counter proves it.
            ++g_confirm_form_requests;
            serve_file(fd, method, "1024", range, false);
        } else if (path == "/stream-forbidden") {
            // Unknown-size probe succeeds (chunked), the streamed GET is refused.
            if (method == "HEAD") {
                send_str(fd, std::string("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n") +
                                 common_headers(0) + "\r\n");
            } else {
                send_json_error(fd, "403 Forbidden", connection_limit_error_body());
            }
        } else {
            const std::string body = "not found";
            send_str(fd, "HTTP/1.1 404 Not Found\r\nContent-Length: " +
                             std::to_string(body.size()) +
                             "\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n" +
                             body);
        }
        close_socket(fd);
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

    // Integer value of a query parameter, `fallback` when it is absent or not a
    // number.
    static int query_int(const std::string& query, const char* name, int fallback) {
        const std::string key = std::string(name) + "=";
        size_t pos = 0;
        while (pos < query.size()) {
            const size_t end = query.find('&', pos);
            const std::string pair =
                query.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
            if (pair.rfind(key, 0) == 0) {
                const std::string value = pair.substr(key.size());
                if (value.empty()) return fallback;
                for (char c : value) {
                    if (c < '0' || c > '9') return fallback;
                }
                return std::atoi(value.c_str());
            }
            if (end == std::string::npos) break;
            pos = end + 1;
        }
        return fallback;
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

    // Google Drive's virus-scan notice: a 200 text/html page whose form carries
    // the confirmation token. With confirm=t the route serves the real file.
    // /drive-loop/ keeps answering with the page, so the "cannot be resolved"
    // path is testable as well.
    static void serve_drive_page(
        socket_t fd,
        const std::string& method, uint16_t port,
                                 const std::string& path, const std::string& query,
                                 const std::string& range) {
        const bool loop = path.rfind("/drive-loop/", 0) == 0;
        const std::string prefix = loop ? "/drive-loop/" : "/drive-page/";
        const std::string size_text = path.substr(prefix.size());
        if (!loop && query.find("confirm=t") != std::string::npos) {
            serve_file(fd, method, size_text, range, false);
            return;
        }
        const std::string action = "http://127.0.0.1:" + std::to_string(port) + path;
        const std::string body =
            "<!DOCTYPE html><html><head><title>Google Drive - Virus scan warning</title>"
            "</head><body><p>Google Drive can't scan this file for viruses.</p>"
            "<form id=\"download-form\" action=\"" +
            action +
            "\" method=\"get\">"
            "<input type=\"submit\" id=\"uc-download-link\" value=\"Download anyway\"/>"
            "<input type=\"hidden\" name=\"id\" value=\"test-file-id\"/>"
            "<input type=\"hidden\" name=\"export\" value=\"download\"/>"
            "<input type=\"hidden\" name=\"confirm\" value=\"t\"/>"
            "<input type=\"hidden\" name=\"uuid\" value=\"0f0f0f0f-1111-2222-3333-444444444444\"/>"
            "</form></body></html>";
        send_str(fd, "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
                     "Content-Length: " +
                         std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
    }

    // Where a private Drive file redirects: Google's sign-in page.
    static void serve_signin_page(socket_t fd) {
        const std::string body =
            "<!DOCTYPE html><html><head><title>Google Drive: Sign-in</title></head><body>"
            "<form action=\"https://accounts.google.com/v3/signin/identifier\" method=\"post\">"
            "<input type=\"email\" id=\"identifierId\" name=\"identifier\"/>"
            "<input type=\"submit\" value=\"Sign in\"/></form>"
            "<p>Use your Google Account (ServiceLogin).</p></body></html>";
        send_str(fd, "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
                     "Content-Length: " +
                         std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
    }

    // An ordinary HTML page from a host that is not a known download
    // interstitial; the page is still downloadable as a file.
    static void serve_plain_page(socket_t fd) {
        const std::string body =
            "<html><head><title>Plain page</title></head><body>hello</body></html>";
        send_str(fd, "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
                     "Content-Length: " +
                         std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
    }

    // A page from a normal host that carries a Drive-shaped confirmation form
    // whose action points at a different origin. The form must not be followed
    // (audit finding 2): `localhost` and `127.0.0.1` are different origins even
    // though they reach the same server, so the counter stays at zero.
    static void serve_confirm_form_page(socket_t fd, uint16_t port) {
        const std::string body =
            "<html><head><title>Not a Drive notice</title></head><body>"
            "<form action=\"http://localhost:" + std::to_string(port) + "/confirm-target\" "
            "method=\"get\">"
            "<input type=\"hidden\" name=\"id\" value=\"test-file-id\"/>"
            "<input type=\"hidden\" name=\"confirm\" value=\"t\"/></form></body></html>";
        send_str(fd, "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
                     "Content-Length: " +
                         std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
    }

    static void serve_file(socket_t fd, const std::string& method, const std::string& size_text,
                           const std::string& range_header, bool slow,
                           const std::string& if_range = {}, bool force_changed = false,
                           uint64_t slow_from = kNoSlowFrom, int slow_ms = 60) {
        const uint64_t total = std::strtoull(size_text.c_str(), nullptr, 10);
        uint64_t start = 0;
        uint64_t end = total == 0 ? 0 : total - 1;
        bool partial = false;
        if (method != "HEAD" && !range_header.empty() && total > 0) {
            partial = parse_ranges(range_header, total, start, end);
        }
        // Throttle only the range that starts at slow_from, so a test can slow
        // one planned segment while a split thief elsewhere runs at full speed.
        const bool throttle = slow || (slow_from != kNoSlowFrom && start == slow_from);
        const bool validators_mismatch = force_changed ||
                                        (!if_range.empty() &&
                                         if_range != "\"etag-" + std::to_string(total) + "\"" &&
                                         if_range != "Wed, 21 Oct 2015 07:28:00 GMT");
        if (partial && validators_mismatch) {
            if (force_changed && method != "HEAD") {
                send_str(fd, "HTTP/1.1 200 OK\r\nContent-Length: " +
                                 std::to_string(total) + "\r\nAccept-Ranges: bytes\r\n" +
                                 common_headers(total + 1) + "\r\n");
                if (total > 0) {
                    std::vector<char> body(65536);
                    for (uint64_t offset = 0; offset < total;) {
                        const uint64_t chunk = std::min<uint64_t>(body.size(), total - offset);
                        for (uint64_t i = 0; i < chunk; ++i) {
                            body[static_cast<size_t>(i)] =
                                static_cast<char>(content_byte(offset + i));
                        }
                        if (!send_all(fd, body.data(), static_cast<size_t>(chunk))) break;
                        offset += chunk;
                    }
                }
            } else {
                send_file_response(fd, method, total, 0, total == 0 ? 0 : total - 1, false,
                                   throttle, if_range, force_changed, slow_ms);
            }
            return;  // validator mismatch returns the complete current entity.
        }
        const int invalid_range_mode = g_invalid_content_range_mode.load();
        if (partial && invalid_range_mode != 0) {
            send_invalid_range_response(fd, method, total, start, end, invalid_range_mode,
                                        if_range);
            return;
        }
        send_file_response(fd, method, total, start, end, partial, throttle, if_range,
                           force_changed, slow_ms);
    }

    static void send_invalid_range_response(socket_t fd, const std::string& method, uint64_t total,
                                            uint64_t start, uint64_t end, int mode,
                                            const std::string& if_range,
                                            bool force_changed = false) {
        uint64_t response_start = start;
        uint64_t response_end = end;
        uint64_t response_total = total;
        if (mode == 1) ++response_start;
        if (mode == 2) --response_end;
        if (mode == 3) ++response_total;
        const uint64_t body_size = end >= start ? end - start + 1 : 0;
        const std::string headers =
            "HTTP/1.1 206 Partial Content\r\nContent-Length: " +
            std::to_string(body_size) + "\r\nContent-Range: bytes " +
            std::to_string(response_start) + "-" + std::to_string(response_end) + "/" +
            std::to_string(response_total) + "\r\nAccept-Ranges: bytes\r\n" +
            common_headers(total, if_range, force_changed) + "\r\n";
        if (!send_str(fd, headers) || method == "HEAD") return;
        std::vector<char> body(static_cast<size_t>(body_size));
        for (uint64_t i = 0; i < body_size; ++i) {
            body[static_cast<size_t>(i)] = static_cast<char>(content_byte(start + i));
        }
        send_all(fd, body.data(), body.size());
    }

    static void send_file_response(socket_t fd, const std::string& method, uint64_t total,
                                   uint64_t start, uint64_t end, bool partial, bool slow,
                                   const std::string& if_range = {}, bool force_changed = false,
                                   int slow_ms = 60) {
        std::string head;
        if (partial) {
            head = "HTTP/1.1 206 Partial Content\r\nContent-Length: " +
                   std::to_string(end - start + 1) +
                   "\r\nContent-Range: bytes " + std::to_string(start) + "-" +
                   std::to_string(end) + "/" + std::to_string(total) +
                    "\r\nAccept-Ranges: bytes\r\n" +
                    common_headers(total, if_range, force_changed) + "\r\n";
        } else {
            head = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(total) +
                   "\r\nAccept-Ranges: bytes\r\n" +
                    common_headers(total, if_range, force_changed) + "\r\n";
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
            if (slow) sleep_ms(slow_ms);
        }
    }

    // One open ranged connection at a time, like a host with a per-IP limit:
    // the probe succeeds, parallel ranges are refused with a JSON 403.
    static void serve_limited_connections(socket_t fd, const std::string& method,
                                          const std::string& range_header,
                                          const std::string& size_text) {
        const uint64_t total = std::strtoull(size_text.c_str(), nullptr, 10);
        if (method == "HEAD" || range_header.empty()) {
            send_file_response(fd, method, total, 0, total == 0 ? 0 : total - 1, false, false);
            return;
        }
        uint64_t start = 0;
        uint64_t end = total == 0 ? 0 : total - 1;
        if (!parse_ranges(range_header, total, start, end)) {
            send_json_error(fd, "403 Forbidden", connection_limit_error_body());
            return;
        }
        if (g_limited_active.fetch_add(1) > 0) {
            g_limited_active.fetch_sub(1);
            g_limited_refusals.fetch_add(1);
            if (::getenv("HTTP_TEST_LOG")) std::fprintf(stderr, "[srv] refuse range=%s\n", range_header.c_str());
            send_json_error(fd, "403 Forbidden", connection_limit_error_body());
            return;
        }
        if (::getenv("HTTP_TEST_LOG")) std::fprintf(stderr, "[srv] accept range=%s\n", range_header.c_str());
        // Hold the connection so the parallel requests overlap deterministically.
        sleep_ms(250);
        send_file_response(fd, method, total, start, end, true, false);
        g_limited_active.fetch_sub(1);
    }

    // UploadHaven's shape: a host that allows `max_active` parallel ranges and
    // answers the rest with 503 plus an HTML "Download In Progress" page. The
    // page carries no Retry-After, exactly like the real host.
    static void serve_limited_connections_503(socket_t fd, const std::string& method,
                                              const std::string& range_header,
                                              const std::string& size_text, int max_active) {
        const uint64_t total = std::strtoull(size_text.c_str(), nullptr, 10);
        if (method == "HEAD" || range_header.empty()) {
            send_file_response(fd, method, total, 0, total == 0 ? 0 : total - 1, false, false);
            return;
        }
        uint64_t start = 0;
        uint64_t end = total == 0 ? 0 : total - 1;
        if (!parse_ranges(range_header, total, start, end)) {
            send_connection_limit_503(fd);
            return;
        }
        const int active = g_limited_503_active.fetch_add(1) + 1;
        if (active > max_active) {
            g_limited_503_active.fetch_sub(1);
            g_limited_503_refusals.fetch_add(1);
            send_connection_limit_503(fd);
            return;
        }
        int peak = g_limited_503_peak.load();
        while (active > peak && !g_limited_503_peak.compare_exchange_weak(peak, active)) {
        }
        // Hold the range so parallel requests overlap deterministically.
        sleep_ms(250);
        send_file_response(fd, method, total, start, end, true, false);
        g_limited_503_active.fetch_sub(1);
    }

    static void send_connection_limit_503(socket_t fd) {
        const std::string body =
            "<html><head><title>Download In Progress - UploadHaven</title></head>"
            "<body><h1>Download already in progress</h1>"
            "<p>You've reached the free download limit. Please wait for an active "
            "download to finish, or upgrade to Pro for parallel downloads.</p>"
            "</body></html>";
        send_str(fd, "HTTP/1.1 503 Service Temporarily Unavailable\r\nContent-Length: " +
                         std::to_string(body.size()) +
                         "\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n" + body);
    }

    // A transient 503 that names its own retry delay (no concurrency wording, so
    // it must take the normal retry path and honour Retry-After).
    static void serve_retry_after_503(socket_t fd, const std::string& method,
                                      const std::string& range_header,
                                      const std::string& size_text) {
        const uint64_t total = std::strtoull(size_text.c_str(), nullptr, 10);
        if (method == "HEAD" || range_header.empty()) {
            send_file_response(fd, method, total, 0, total == 0 ? 0 : total - 1, false, false);
            return;
        }
        if (g_retry_after_refusals.fetch_add(1) < 1) {
            const std::string body = "the origin is warming up";
            send_str(fd, "HTTP/1.1 503 Service Unavailable\r\nRetry-After: 1\r\nContent-Length: " +
                             std::to_string(body.size()) +
                             "\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n" + body);
            return;
        }
        uint64_t start = 0;
        uint64_t end = total == 0 ? 0 : total - 1;
        parse_ranges(range_header, total, start, end);
        send_file_response(fd, method, total, start, end, true, false);
    }

    static void send_json_error(socket_t fd, const char* status, const std::string& body) {
        send_str(fd, std::string("HTTP/1.1 ") + status + "\r\nContent-Length: " +
                         std::to_string(body.size()) +
                         "\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n" +
                         body);
    }

    // Faithful shape of pixeldrain's hotlink refusal.
    static std::string hotlink_error_body() {
        return "{\"success\":false,\"value\":\"hotlink_detected\",\"message\":\"Hotlinking was "
               "detected for this download, hotlinking is only allowed on premium accounts\"}";
    }

    static std::string connection_limit_error_body() {
        return "{\"success\":false,\"value\":\"max_concurrent_downloads\",\"message\":\"You have "
               "reached the maximum number of open download connections for free accounts\"}";
    }

    static void serve_chunked(socket_t fd, const std::string& method,
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

    static std::string hls_media_playlist(int variant, int segments) {
        std::string text = "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:4\n";
        for (int s = 0; s < segments; ++s) {
            text += "#EXTINF:4.0,\nseg-" + std::to_string(variant) + "-" +
                    std::to_string(s) + ".ts\n";
        }
        text += "#EXT-X-ENDLIST\n";
        return text;
    }

    static void send_playlist(socket_t fd, const std::string& method, const std::string& body) {
        send_str(fd, "HTTP/1.1 200 OK\r\nContent-Type: application/vnd.apple.mpegurl"
                     "\r\nContent-Length: " +
                         std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n");
        if (method != "HEAD") send_str(fd, body);
    }

    static void serve_hls(socket_t fd, const std::string& method, const std::string& path) {
        if (path == "/hls/master.m3u8") {
            send_playlist(fd, method,
                          "#EXTM3U\n"
                          "#EXT-X-STREAM-INF:BANDWIDTH=500000,RESOLUTION=640x360\n"
                          "variant-1.m3u8\n"
                          "#EXT-X-STREAM-INF:BANDWIDTH=2000000,RESOLUTION=1280x720\n"
                          "variant-2.m3u8\n");
            return;
        }
        if (path == "/hls/encrypted.m3u8") {
            send_playlist(fd, method,
                          "#EXTM3U\n"
                          "#EXT-X-KEY:METHOD=AES-128,URI=\"key.bin\"\n"
                          "#EXTINF:4.0,\nseg-1-0.ts\n"
                          "#EXT-X-ENDLIST\n");
            return;
        }
        if (path == "/hls/map.m3u8") {
            send_playlist(fd, method,
                          "#EXTM3U\n"
                          "#EXT-X-MAP:URI=\"init.mp4\"\n"
                          "#EXTINF:4.0,\nseg-1-0.ts\n"
                          "#EXTINF:4.0,\nseg-2-1.ts\n"
                          "#EXT-X-ENDLIST\n");
            return;
        }
        // Scheme allowlist fixtures (security regression): a playlist must not
        // be able to point the transfer at a local file.
        if (path == "/hls/file-segment.m3u8") {
            send_playlist(fd, method,
                          "#EXTM3U\n"
                          "#EXT-X-TARGETDURATION:10\n"
                          "#EXTINF:10,\n"
                          "file:///etc/passwd\n"
                          "#EXT-X-ENDLIST\n");
            return;
        }
        if (path == "/hls/file-variant.m3u8") {
            send_playlist(fd, method,
                          "#EXTM3U\n"
                          "#EXT-X-STREAM-INF:BANDWIDTH=500000\n"
                          "file:///etc/passwd\n");
            return;
        }
        if (path == "/hls/file-map.m3u8") {
            send_playlist(fd, method,
                          "#EXTM3U\n"
                          "#EXT-X-MAP:URI=\"file:///etc/passwd\"\n"
                          "#EXTINF:4.0,\nseg-1-0.ts\n"
                          "#EXT-X-ENDLIST\n");
            return;
        }
        if (path == "/hls/init.mp4") {
            serve_hls_segment(fd, method, 0, 0);
            return;
        }
        if (path.rfind("/hls/variant-", 0) == 0 && path.size() >= 14) {
            const int variant = path[13] - '0';
            send_playlist(fd, method, hls_media_playlist(variant, 3));
            return;
        }
        if (path.rfind("/hls/seg-", 0) == 0) {
            const std::string rest = path.substr(9);  // "<v>-<s>.ts"
            const int variant = std::atoi(rest.c_str());
            const size_t dash = rest.find('-');
            const int segment =
                dash == std::string::npos ? 0 : std::atoi(rest.substr(dash + 1).c_str());
            serve_hls_segment(fd, method, variant, segment);
            return;
        }
        send_str(fd, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
    }

    // DASH fixture: SegmentTemplate + SegmentTimeline, two video renditions and
    // one audio rendition, four segments each. Bodies are deterministic.
    static std::string dash_manifest() {
        return "<?xml version=\"1.0\"?>\n"
               "<MPD type=\"static\" mediaPresentationDuration=\"PT8S\">\n"
               "  <Period>\n"
               "    <AdaptationSet contentType=\"video\" mimeType=\"video/mp4\">\n"
               "      <SegmentTemplate timescale=\"90000\" duration=\"180000\" startNumber=\"1\" "
               "initialization=\"init-$RepresentationID$.mp4\" "
               "media=\"seg-$RepresentationID$-$Number$.m4s\">\n"
               "        <SegmentTimeline>\n"
               "          <S t=\"0\" d=\"180000\" r=\"3\"/>\n"
               "        </SegmentTimeline>\n"
               "      </SegmentTemplate>\n"
               "      <Representation id=\"v1\" bandwidth=\"5000000\" width=\"1920\" height=\"1080\" "
               "codecs=\"avc1.640028\"/>\n"
               "      <Representation id=\"v2\" bandwidth=\"2500000\" width=\"1280\" height=\"720\" "
               "codecs=\"avc1.4d401f\"/>\n"
               "    </AdaptationSet>\n"
               "    <AdaptationSet contentType=\"audio\" mimeType=\"audio/mp4\">\n"
               "      <SegmentTemplate timescale=\"48000\" duration=\"96000\" startNumber=\"1\" "
               "initialization=\"init-$RepresentationID$.mp4\" "
               "media=\"seg-$RepresentationID$-$Number$.m4s\">\n"
               "        <SegmentTimeline>\n"
               "          <S t=\"0\" d=\"96000\" r=\"3\"/>\n"
               "        </SegmentTimeline>\n"
               "      </SegmentTemplate>\n"
               "      <Representation id=\"a1\" bandwidth=\"128000\" codecs=\"mp4a.40.2\"/>\n"
               "    </AdaptationSet>\n"
               "  </Period>\n"
               "</MPD>\n";
    }

    static std::string dash_segment_body(const std::string& path) {
        // /dash/init-<rep>.mp4 or /dash/seg-<rep>-<n>.m4s
        int rep_index = 0;
        int number = 0;
        if (path.rfind("/dash/init-", 0) == 0) {
            const std::string rep = path.substr(11, path.size() - 11 - 4);
            rep_index = rep == "v1" ? 0 : (rep == "v2" ? 1 : 2);
        } else if (path.rfind("/dash/seg-", 0) == 0) {
            const size_t dot = path.rfind('.');
            const std::string rest = path.substr(10, dot - 10);
            const size_t dash = rest.rfind('-');
            if (dash == std::string::npos) return {};
            const std::string rep = rest.substr(0, dash);
            rep_index = rep == "v1" ? 0 : (rep == "v2" ? 1 : 2);
            number = std::atoi(rest.substr(dash + 1).c_str());
        } else {
            return {};
        }
        const uint64_t offset = static_cast<uint64_t>(rep_index) * 100000u +
                                static_cast<uint64_t>(number) * 16u;
        std::string body;
        for (int i = 0; i < 64; ++i) {
            body.push_back(static_cast<char>(content_byte(offset + static_cast<uint64_t>(i))));
        }
        return body;
    }

    static void serve_dash(socket_t fd, const std::string& method, const std::string& path) {
        if (path == "/dash/manifest.mpd") {
            const std::string body = dash_manifest();
            send_str(fd, "HTTP/1.1 200 OK\r\nContent-Type: application/dash+xml\r\n"
                         "Content-Length: " +
                         std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n");
            if (method != "HEAD") send_str(fd, body);
            return;
        }
        const std::string body = dash_segment_body(path);
        if (body.empty()) {
            send_str(fd, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
            return;
        }
        send_str(fd, "HTTP/1.1 200 OK\r\nContent-Type: video/iso.segment\r\n"
                     "Content-Length: " +
                     std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n");
        if (method != "HEAD") send_str(fd, body);
    }

    // Builds one ISOBMFF media segment: moof(mfhd sequence, traf/tfdt) + mdat.
    // The box layout matches what engine/mp4_boxes.cpp reads back.
    static std::vector<uint8_t> mse_segment_bytes(uint32_t sequence, uint64_t base_decode_time) {
        std::vector<uint8_t> out;
        auto put_u32 = [](std::vector<uint8_t>& dst, uint32_t v) {
            dst.push_back(static_cast<uint8_t>(v >> 24));
            dst.push_back(static_cast<uint8_t>(v >> 16));
            dst.push_back(static_cast<uint8_t>(v >> 8));
            dst.push_back(static_cast<uint8_t>(v));
        };
        auto put_u64 = [&put_u32](std::vector<uint8_t>& dst, uint64_t v) {
            put_u32(dst, static_cast<uint32_t>(v >> 32));
            put_u32(dst, static_cast<uint32_t>(v));
        };
        auto put_type = [](std::vector<uint8_t>& dst, const char* type) {
            dst.insert(dst.end(), {static_cast<uint8_t>(type[0]),
                                   static_cast<uint8_t>(type[1]),
                                   static_cast<uint8_t>(type[2]),
                                   static_cast<uint8_t>(type[3])});
        };

        std::vector<uint8_t> mfhd;
        put_u32(mfhd, 16);
        put_type(mfhd, "mfhd");
        put_u32(mfhd, 0);
        put_u32(mfhd, sequence);

        std::vector<uint8_t> tfdt;
        put_u32(tfdt, 20);
        put_type(tfdt, "tfdt");
        tfdt.push_back(1);
        tfdt.push_back(0);
        tfdt.push_back(0);
        tfdt.push_back(0);
        put_u64(tfdt, base_decode_time);

        std::vector<uint8_t> traf;
        put_u32(traf, 8 + static_cast<uint32_t>(tfdt.size()));
        put_type(traf, "traf");
        traf.insert(traf.end(), tfdt.begin(), tfdt.end());

        std::vector<uint8_t> moof;
        put_u32(moof, 8 + static_cast<uint32_t>(mfhd.size()) +
                          static_cast<uint32_t>(traf.size()));
        put_type(moof, "moof");
        moof.insert(moof.end(), mfhd.begin(), mfhd.end());
        moof.insert(moof.end(), traf.begin(), traf.end());

        out.insert(out.end(), moof.begin(), moof.end());
        std::vector<uint8_t> mdat;
        put_u32(mdat, 8 + 16);
        put_type(mdat, "mdat");
        for (int i = 0; i < 16; ++i) {
            mdat.push_back(content_byte(static_cast<uint64_t>(sequence) * 16u +
                                        static_cast<uint64_t>(i)));
        }
        out.insert(out.end(), mdat.begin(), mdat.end());
        return out;
    }

    static void serve_mse(socket_t fd, const std::string& method, const std::string& path) {
        uint32_t sequence = 0;
        uint64_t base_decode_time = 0;
        bool known = false;
        if (path.rfind("/mse/seg-", 0) == 0) {
            const int n = std::atoi(path.c_str() + 9);
            sequence = static_cast<uint32_t>(n);
            base_decode_time = static_cast<uint64_t>(n) * 90000u;
            known = true;
        } else if (path.rfind("/mse/gap-", 0) == 0) {
            // Sequence numbers jump by two so the continuity check must fail.
            const int n = std::atoi(path.c_str() + 9);
            sequence = static_cast<uint32_t>(n) * 2u;
            base_decode_time = static_cast<uint64_t>(n) * 180000u;
            known = true;
        }
        if (!known) {
            send_str(fd, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
            return;
        }
        const std::vector<uint8_t> body = mse_segment_bytes(sequence, base_decode_time);
        send_str(fd, "HTTP/1.1 200 OK\r\nContent-Type: video/iso.segment\r\n"
                     "Content-Length: " +
                     std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n");
        if (method != "HEAD") send_all(fd, body.data(), body.size());
    }

    static void serve_hls_segment(socket_t fd, const std::string& method, int variant, int segment) {
        send_str(fd, "HTTP/1.1 200 OK\r\nContent-Type: video/mp2t\r\nAccept-Ranges: bytes"
                     "\r\nContent-Length: " +
                         std::to_string(kHlsSegmentSize) + "\r\nConnection: close\r\n\r\n");
        if (method == "HEAD") return;
        std::vector<char> buf(static_cast<size_t>(kHlsSegmentSize));
        for (uint64_t i = 0; i < kHlsSegmentSize; ++i) {
            buf[static_cast<size_t>(i)] =
                static_cast<char>(hls_segment_byte(variant, segment, i));
        }
        send_all(fd, buf.data(), buf.size());
    }
};

}  // namespace testutil
