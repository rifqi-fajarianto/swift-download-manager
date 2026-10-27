// CLI/backend tests for the FTP and HLS download paths (no GUI, per AGENTS.md).
// A local FTP server (SIZE/REST/EPSV) and the shared HTTP test server (HLS
// playlist + segments) provide the remote side.

#include <cstdio>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sys/stat.h>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "engine/download_engine.h"
#include "engine/hls.h"
#include "ftp_test_server.h"
#include "http_test_server.h"
#include "net/http.h"
#include "test_util.h"

using namespace swiftdm;

namespace {

Settings test_settings(const std::string& dir) {
    Settings s = default_settings();
    s.download_dir = dir + "/out";
    s.temp_dir = dir + "/tmp";
    s.category_folders = false;
    s.default_agent = "SwiftDM-Test/1.0";
    return s;
}

std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in),
                                std::istreambuf_iterator<char>());
}

void test_playlist_parsing() {
    HlsPlaylist playlist;
    std::string error;

    CHECK(parse_hls_playlist(
        "#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=500000,RESOLUTION=640x360\nlow.m3u8\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=2000000\nhigh.m3u8\n",
        &playlist, &error));
    CHECK(playlist.master);
    CHECK_EQ(playlist.variants.size(), size_t(2));
    CHECK_EQ(playlist.variants[1].first, int64_t(2000000));
    CHECK_EQ(playlist.variants[1].second, std::string("high.m3u8"));

    CHECK(parse_hls_playlist(
        "#EXTM3U\n#EXTINF:4.0,\nseg1.ts\n#EXTINF:4.0,\nhttps://cdn.example/seg2.ts\n",
        &playlist, &error));
    CHECK(!playlist.master);
    CHECK_EQ(playlist.segments.size(), size_t(2));
    CHECK_EQ(playlist.segments[1], std::string("https://cdn.example/seg2.ts"));

    CHECK(parse_hls_playlist("#EXTM3U\n#EXT-X-MAP:URI=\"init.mp4\"\n#EXTINF:4,\nseg.ts\n",
                             &playlist, &error));
    CHECK_EQ(playlist.init_segment, std::string("init.mp4"));

    CHECK(parse_hls_playlist(
        "#EXTM3U\n#EXT-X-KEY:METHOD=AES-128,URI=\"key.bin\"\n#EXTINF:4,\nseg.ts\n", &playlist,
        &error));
    CHECK(playlist.encrypted);

    CHECK(parse_hls_playlist(
        "#EXTM3U\n#EXT-X-KEY:METHOD=NONE\n#EXTINF:4,\nseg.ts\n", &playlist, &error));
    CHECK(!playlist.encrypted);

    CHECK(!parse_hls_playlist("<html><body>nope</body></html>", &playlist, &error));
    CHECK(!error.empty());

    CHECK_EQ(resolve_playlist_uri("http://h/a/b.m3u8", "c.ts"), std::string("http://h/a/c.ts"));
    CHECK_EQ(resolve_playlist_uri("http://h/a/b.m3u8", "/c.ts"), std::string("http://h/c.ts"));
    CHECK_EQ(resolve_playlist_uri("http://h/a/b.m3u8", "//cdn.example/c.ts"),
             std::string("http://cdn.example/c.ts"));
    CHECK_EQ(resolve_playlist_uri("http://h/a/b.m3u8", "https://cdn.example/c.ts"),
             std::string("https://cdn.example/c.ts"));

    CHECK(is_hls_url("http://h/play.m3u8?token=1", ""));
    CHECK(is_hls_url("http://h/play", "application/vnd.apple.mpegurl"));
    CHECK(!is_hls_url("http://h/file.mp4", "video/mp4"));

    CHECK(is_ftp_url("ftp://example.com/file.bin"));
    CHECK(is_ftp_url("FTP://example.com/file.bin"));
    CHECK(!is_ftp_url("http://example.com/file.bin"));
}

// Highest bandwidth variant (2), three 16 KiB segments.
std::vector<uint8_t> expected_hls_bytes(int variant = 2) {
    std::vector<uint8_t> expected;
    expected.reserve(static_cast<size_t>(testutil::kHlsSegmentSize * 3));
    for (int segment = 0; segment < 3; ++segment) {
        for (uint64_t i = 0; i < testutil::kHlsSegmentSize; ++i) {
            expected.push_back(testutil::hls_segment_byte(variant, segment, i));
        }
    }
    return expected;
}

void test_hls_download(const std::string& dir, testutil::TestHttpServer& server) {
    Settings settings = test_settings(dir);
    DownloadEngine engine(settings);

    DownloadRequest request;
    request.url = server.base_url() + "/hls/master.m3u8";
    request.connections = 4;  // must still take the HLS path

    const auto result = engine.run(request);
    CHECK(result.status == DownloadResult::Status::Completed);
    CHECK(result.path.find("master.ts") != std::string::npos);
    CHECK_EQ(result.size, uint64_t(expected_hls_bytes().size()));
    CHECK(read_file(result.path) == expected_hls_bytes());
    CHECK(std::filesystem::is_empty(settings.temp_dir));
}

void test_hls_live_bandwidth_limit(const std::string& dir,
                                   testutil::TestHttpServer& server) {
    Settings settings = test_settings(dir);
    DownloadEngine engine(settings);
    DownloadRequest request;
    request.url = server.base_url() + "/hls/master.m3u8";
    request.bandwidth_limit_enabled = true;
    request.bandwidth_limit_kbps = 8;
    request.live_bandwidth_limit = std::make_shared<BandwidthLimit>(8 * 1024);
    const auto start = std::chrono::steady_clock::now();
    std::thread editor([&] {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        request.live_bandwidth_limit->set_bytes_per_second(0);
    });
    const auto result = engine.run(request);
    editor.join();
    const double seconds = std::chrono::duration<double>(
                               std::chrono::steady_clock::now() - start)
                               .count();
    CHECK(result.status == DownloadResult::Status::Completed);
    CHECK(read_file(result.path) == expected_hls_bytes());
    CHECK(seconds < 5.0);
}

void test_hls_resume(const std::string& dir, testutil::TestHttpServer& server) {
    Settings settings = test_settings(dir);
    DownloadEngine engine(settings);

    DownloadRequest request;
    request.url = server.base_url() + "/hls/master.m3u8";

    // Stop after the first segment (33%).
    const auto first = engine.run(request, [](const ProgressInfo& info) {
        return info.percent < 33;
    });
    CHECK(first.status == DownloadResult::Status::Paused);
    CHECK(first.path.find(".part") != std::string::npos);
#ifdef _WIN32
    // Windows has no POSIX mode bits; the workspace just has to exist.
    CHECK(std::filesystem::is_directory(path_from_utf8(settings.temp_dir)));
    CHECK(std::filesystem::is_directory(path_from_utf8(first.workspace_dir)));
    CHECK(std::filesystem::exists(path_from_utf8(first.path)));
#else
    struct stat temp_dir_stat {};
    CHECK(::stat(settings.temp_dir.c_str(), &temp_dir_stat) == 0);
    CHECK((temp_dir_stat.st_mode & 0777) == 0700);
    struct stat workspace_stat {};
    CHECK(::stat(first.workspace_dir.c_str(), &workspace_stat) == 0);
    CHECK((workspace_stat.st_mode & 0777) == 0700);
    struct stat part_stat {};
    CHECK(::stat(first.path.c_str(), &part_stat) == 0);
    CHECK((part_stat.st_mode & 0777) == 0644);
#endif
    const auto partial = read_file(first.path);
    CHECK_EQ(partial.size(), testutil::kHlsSegmentSize);

    const auto second = engine.run(request);
    CHECK(second.status == DownloadResult::Status::Completed);
    CHECK(read_file(second.path) == expected_hls_bytes());
    CHECK(std::filesystem::is_empty(settings.temp_dir));
}

void test_hls_encrypted_is_refused(const std::string& dir,
                                   testutil::TestHttpServer& server) {
    Settings settings = test_settings(dir);
    DownloadEngine engine(settings);

    DownloadRequest request;
    request.url = server.base_url() + "/hls/encrypted.m3u8";

    const auto result = engine.run(request);
    CHECK(result.status == DownloadResult::Status::Failed);
    CHECK(result.error.find("encrypted") != std::string::npos);
}

// A playlist is remote input: file:// segments, file:// variants and a
// file:// EXT-X-MAP must fail before anything is written (audit finding 1).
void test_hls_unsupported_scheme(const std::string& dir,
                                 testutil::TestHttpServer& server) {
    for (const std::string& route : {std::string("/hls/file-segment.m3u8"),
                                     std::string("/hls/file-variant.m3u8"),
                                     std::string("/hls/file-map.m3u8")}) {
        Settings settings = test_settings(dir + route);
        DownloadEngine engine(settings);

        DownloadRequest request;
        request.url = server.base_url() + route;

        const auto result = engine.run(request);
        CHECK(result.status == DownloadResult::Status::Failed);
        CHECK(result.error.find("unsupported URL scheme") != std::string::npos);
        CHECK(result.path.empty());
        // The local file must never reach the destination directory.
        std::error_code ec;
        CHECK(std::filesystem::is_empty(settings.download_dir, ec));
    }
}

std::vector<uint8_t> expected_ftp_bytes() {
    std::vector<uint8_t> expected;
    expected.reserve(static_cast<size_t>(testutil::kFtpFileSize));
    for (uint64_t i = 0; i < testutil::kFtpFileSize; ++i) {
        expected.push_back(testutil::content_byte(i));
    }
    return expected;
}

void test_ftp_probe_and_download(const std::string& dir, testutil::TestFtpServer& server) {
    Settings settings = test_settings(dir);
    DownloadEngine engine(settings);

    HttpOptions probe_options;
    probe_options.url = server.url("/ftp-file.bin");
    const ProbeResult probe = probe_url(probe_options);
    CHECK(probe.ok);
    CHECK_EQ(probe.content_length, server.file_size());

    DownloadRequest request;
    request.url = server.url("/ftp-file.bin");
    request.connections = 4;  // FTP stays on a single connection
    const auto result = engine.run(request);
    CHECK(result.status == DownloadResult::Status::Completed);
    CHECK(result.path.find("ftp-file.bin") != std::string::npos);
    CHECK_EQ(result.size, server.file_size());
    CHECK(read_file(result.path) == expected_ftp_bytes());
    CHECK(server.retr_commands() >= 1);
}

void test_ftp_live_bandwidth_limit(const std::string& dir, testutil::TestFtpServer& server) {
    Settings settings = test_settings(dir);
    DownloadEngine engine(settings);
    DownloadRequest request;
    request.url = server.url("/ftp-file.bin");
    request.bandwidth_limit_enabled = true;
    request.bandwidth_limit_kbps = 8;
    request.live_bandwidth_limit = std::make_shared<BandwidthLimit>(8 * 1024);
    const auto start = std::chrono::steady_clock::now();
    std::thread editor([&] {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        request.live_bandwidth_limit->set_bytes_per_second(1024 * 1024);
    });
    const auto result = engine.run(request);
    editor.join();
    const double seconds = std::chrono::duration<double>(
                               std::chrono::steady_clock::now() - start)
                               .count();
    CHECK(result.status == DownloadResult::Status::Completed);
    CHECK(read_file(result.path) == expected_ftp_bytes());
    CHECK(seconds < 8.0);
}

void test_ftp_resume_uses_rest(const std::string& dir, testutil::TestFtpServer& server) {
    Settings settings = test_settings(dir);
    DownloadEngine engine(settings);

    DownloadRequest request;
    request.url = server.url("/ftp-file.bin");

    // Simulate an interrupted transfer: place the first 64 KiB in the workspace
    // and run the engine again; the FTP path must continue with REST.
    const std::string filename = "ftp-file.bin";
    const std::string effective = request.url;  // no redirects over FTP
    const DownloadWorkspace ws = make_workspace(settings, effective, filename);
    std::error_code ec;
    std::filesystem::create_directories(ws.dir, ec);
    {
        const auto expected = expected_ftp_bytes();
        std::ofstream out(ws.part_path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(expected.data()), 64 * 1024);
    }

    const int rest_before = server.rest_commands();
    const auto result = engine.run(request);
    CHECK(result.status == DownloadResult::Status::Completed);
    CHECK(server.rest_commands() > rest_before);
    CHECK(read_file(result.path) == expected_ftp_bytes());
}

}  // namespace

int main() {
    const std::string dir = testutil::make_temp_dir("media");

    testutil::TestHttpServer http;
    CHECK(http.start());
    testutil::TestFtpServer ftp;
    CHECK(ftp.start());

    test_playlist_parsing();
    test_hls_download(dir, http);
    test_hls_live_bandwidth_limit(dir + "/hls-live-limit", http);
    test_hls_resume(dir, http);
    test_hls_encrypted_is_refused(dir, http);
    test_hls_unsupported_scheme(dir, http);
    test_ftp_probe_and_download(dir, ftp);
    test_ftp_live_bandwidth_limit(dir + "/ftp-live-limit", ftp);
    test_ftp_resume_uses_rest(dir, ftp);

    testutil::cleanup_dir(dir);
    return testutil::finish("media");
}
