#pragma once

// Minimal HTTP/1.1 test server for CLI/backend integration tests.
// Routes:
//   GET/HEAD /file/<size>[?slow=1]  ranges supported, deterministic content
//   GET/HEAD /space-file/<name>     fixed file; exercises escaped path spaces
//   GET/HEAD /chunked/<size>        chunked transfer, no ranges
//   GET/HEAD /redirect/<size>       302 to /file/<size>
//   GET/HEAD /forbidden             403 response
//   GET/HEAD /json-error            403 with a JSON error body (pixeldrain shape)
//   GET/HEAD /segments-forbidden/N  200 probe, ranged GETs answer 403 JSON
//   GET/HEAD /stream-forbidden      200 chunked probe, streamed GET answers 403 JSON
//   GET/HEAD /limit-connections/N   200 probe, parallel ranges answer 403 JSON
//   GET/HEAD /drive-page/N          Google Drive virus-scan notice; with
//                                   confirm=t the same route serves /file/N
//   GET/HEAD /drive-loop/N          virus-scan notice that never resolves
//   GET/HEAD /signin                Google sign-in page (private-file wall)
//   GET/HEAD /page                  plain HTML page (no file download)
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

inline std::atomic<int> g_cookie_requests{0};
inline std::atomic<int> g_non_matching_cookie_requests{0};
inline std::atomic<int> g_head_requests{0};
inline std::atomic<int> g_limited_active{0};
inline std::atomic<int> g_limited_refusals{0};

inline uint8_t content_byte(uint64_t i) {
    uint32_t x = static_cast<uint32_t>(i) * 2654435761u + 12345u;
    x ^= x >> 13;
    x *= 2246822519u;
    x ^= x >> 16;
    return static_cast<uint8_t>(x & 0xff);
}

// HLS fixtures: every segment is 16 KiB and distinguishable by variant/index.
inline constexpr uint64_t kHlsSegmentSize = 16 * 1024;

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

  public:
    int cookie_requests() const { return g_cookie_requests.load(); }
    int non_matching_cookie_requests() const { return g_non_matching_cookie_requests.load(); }
    int limited_refusals() const { return g_limited_refusals.load(); }
    int head_requests() const { return g_head_requests.load(); }
    void reset_limited_refusals() { g_limited_refusals.store(0); }

  private:
    void accept_loop() {
        while (running_.load()) {
            sockaddr_in peer{};
            socklen_t len = sizeof(peer);
            const int fd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&peer), &len);
            if (fd < 0) break;
            std::thread([this, fd] { handle_conn(fd, port_); }).detach();
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

    static void handle_conn(int fd, uint16_t port) {
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
        if (method == "HEAD") g_head_requests.fetch_add(1);

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

        if (path.rfind("/file/", 0) == 0) {
            serve_file(fd, method, path.substr(6), range, slow);
        } else if (path.rfind("/space-file/", 0) == 0) {
            serve_file(fd, method, "65536", range, false);
        } else if (path.rfind("/chunked/", 0) == 0) {
            serve_chunked(fd, method, path.substr(9));
        } else if (path.rfind("/redirect/", 0) == 0) {
            const std::string location = "/file/" + path.substr(10);
            send_str(fd, "HTTP/1.1 302 Found\r\nLocation: " + location +
                             "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
        } else if (path.rfind("/hls/", 0) == 0) {
            serve_hls(fd, method, path);
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
        } else if (path.rfind("/drive-page/", 0) == 0 || path.rfind("/drive-loop/", 0) == 0) {
            serve_drive_page(fd, method, port, path, query, range);
        } else if (path == "/signin") {
            serve_signin_page(fd);
        } else if (path == "/page") {
            serve_plain_page(fd);
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

    // Google Drive's virus-scan notice: a 200 text/html page whose form carries
    // the confirmation token. With confirm=t the route serves the real file.
    // /drive-loop/ keeps answering with the page, so the "cannot be resolved"
    // path is testable as well.
    static void serve_drive_page(int fd, const std::string& method, uint16_t port,
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
    static void serve_signin_page(int fd) {
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
    static void serve_plain_page(int fd) {
        const std::string body =
            "<html><head><title>Plain page</title></head><body>hello</body></html>";
        send_str(fd, "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
                     "Content-Length: " +
                         std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
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
        send_file_response(fd, method, total, start, end, partial, slow);
    }

    static void send_file_response(int fd, const std::string& method, uint64_t total,
                                   uint64_t start, uint64_t end, bool partial, bool slow) {
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

    // One open ranged connection at a time, like a host with a per-IP limit:
    // the probe succeeds, parallel ranges are refused with a JSON 403.
    static void serve_limited_connections(int fd, const std::string& method,
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
        ::usleep(250000);
        send_file_response(fd, method, total, start, end, true, false);
        g_limited_active.fetch_sub(1);
    }

    static void send_json_error(int fd, const char* status, const std::string& body) {
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

    static std::string hls_media_playlist(int variant, int segments) {
        std::string text = "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:4\n";
        for (int s = 0; s < segments; ++s) {
            text += "#EXTINF:4.0,\nseg-" + std::to_string(variant) + "-" +
                    std::to_string(s) + ".ts\n";
        }
        text += "#EXT-X-ENDLIST\n";
        return text;
    }

    static void send_playlist(int fd, const std::string& method, const std::string& body) {
        send_str(fd, "HTTP/1.1 200 OK\r\nContent-Type: application/vnd.apple.mpegurl"
                     "\r\nContent-Length: " +
                         std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n");
        if (method != "HEAD") send_str(fd, body);
    }

    static void serve_hls(int fd, const std::string& method, const std::string& path) {
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

    static void serve_hls_segment(int fd, const std::string& method, int variant, int segment) {
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
