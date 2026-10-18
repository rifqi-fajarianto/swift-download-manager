#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "engine/categories.h"
#include "engine/download_engine.h"
#include "engine/segment_plan.h"
#include "http_test_server.h"
#include "net/http.h"
#include "test_util.h"

using namespace swiftdm;

namespace {

constexpr uint64_t k5MB = 5u << 20;

Settings test_settings(const std::string& dir) {
    Settings s = default_settings();
    s.download_dir = dir + "/out";
    s.temp_dir = dir + "/tmp";
    s.category_folders = false;
    s.max_connections = 4;
    s.default_agent = "SwiftDM-Test/1.0";
    return s;
}

bool file_matches(const std::string& path, uint64_t size) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::vector<char> buf(1 << 16);
    uint64_t offset = 0;
    while (in) {
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        const std::streamsize got = in.gcount();
        for (std::streamsize i = 0; i < got; ++i) {
            if (static_cast<uint8_t>(buf[static_cast<size_t>(i)]) !=
                testutil::content_byte(offset + static_cast<uint64_t>(i))) {
                return false;
            }
        }
        offset += static_cast<uint64_t>(got);
    }
    return offset == size;
}

bool dir_empty(const std::string& dir) {
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) return true;
    return std::filesystem::is_empty(dir, ec);
}

void test_probe(testutil::TestHttpServer& server) {
    HttpOptions opts;
    opts.url = server.base_url() + "/file/1048576";
    opts.user_agent = "SwiftDM-Test/1.0";
    const ProbeResult probe = probe_url(opts);
    CHECK(probe.ok);
    CHECK_EQ(probe.content_length, uint64_t(1048576));
    CHECK(probe.accept_ranges);
    CHECK_EQ(probe.filename_hint, std::string("sample-1048576.bin"));
    CHECK(probe.etag.find("etag-1048576") != std::string::npos);
}

void test_browser_cookie_scope(const std::string& dir, testutil::TestHttpServer& server) {
    const Settings settings = test_settings(dir);
    DownloadEngine engine(settings);
    const std::string url = server.base_url() + "/file/65536";
    const std::string host = "127.0.0.1";

    DownloadRequest request;
    request.url = url;
    request.cookies.push_back({host, "/file", "swift_test", "secret", false, 0});
    request.cookies.push_back({"other.invalid", "/", "other_test", "private", false, 0});

    const int matching_before = server.cookie_requests();
    const int non_matching_before = server.non_matching_cookie_requests();
    const auto result = engine.run(request);
    CHECK(result.status == DownloadResult::Status::Completed);
    CHECK(server.cookie_requests() > matching_before);
    CHECK_EQ(server.non_matching_cookie_requests(), non_matching_before);
}

void test_download_segmented(const std::string& dir, testutil::TestHttpServer& server) {
    const Settings settings = test_settings(dir);
    DownloadEngine engine(settings);

    DownloadRequest req;
    req.url = server.base_url() + "/file/" + std::to_string(k5MB);
    req.connections = 4;

    const auto result = engine.run(req);

    CHECK(result.status == DownloadResult::Status::Completed);
    CHECK(result.path.find("sample-" + std::to_string(k5MB) + ".bin") != std::string::npos);
    CHECK_EQ(result.size, k5MB);
    CHECK(file_matches(result.path, k5MB));
    CHECK(dir_empty(settings.temp_dir));  // workspace cleaned up
}

void test_download_single_connection(const std::string& dir,
                                     testutil::TestHttpServer& server) {
    const Settings settings = test_settings(dir);
    DownloadEngine engine(settings);

    DownloadRequest req;
    req.url = server.base_url() + "/file/1048576";
    req.connections = 1;

    const auto result = engine.run(req);
    CHECK(result.status == DownloadResult::Status::Completed);
    CHECK_EQ(result.size, uint64_t(1048576));
    CHECK(file_matches(result.path, 1048576));
}

void test_resume_after_pause(const std::string& dir, testutil::TestHttpServer& server) {
    const Settings settings = test_settings(dir);
    DownloadEngine engine(settings);

    DownloadRequest req;
    req.url = server.base_url() + "/file/" + std::to_string(k5MB) + "?slow=1";
    req.connections = 4;

    uint64_t paused_done = 0;
    const auto first = engine.run(req, [&](const ProgressInfo& info) {
        paused_done = info.done;
        return info.done < (1u << 20);  // pause after ~1 MB
    });
    CHECK(first.status == DownloadResult::Status::Paused);
    CHECK(!first.path.empty());
    CHECK(std::filesystem::exists(first.path));
    CHECK(paused_done >= (1u << 20));
    CHECK(paused_done < k5MB);

    // State must be persisted for resume.
    const DownloadWorkspace ws = make_workspace(
        settings, req.url, "sample-" + std::to_string(k5MB) + ".bin");
    TransferState state;
    CHECK(load_transfer_state(ws.state_path, state, nullptr));
    CHECK_EQ(state.file_size, k5MB);
    CHECK(state.done_bytes() > 0);
    CHECK(state.done_bytes() < k5MB);

    uint64_t first_report = 0;
    uint64_t last_seen = 0;
    bool monotonic = true;
    const auto second = engine.run(req, [&](const ProgressInfo& info) {
        if (first_report == 0) first_report = info.done;
        if (info.done < last_seen) monotonic = false;
        last_seen = info.done;
        return true;
    });
    CHECK(second.status == DownloadResult::Status::Completed);
    CHECK_EQ(second.size, k5MB);
    CHECK(file_matches(second.path, k5MB));
    CHECK(first_report >= state.done_bytes());  // resumed from stored progress
    CHECK(monotonic);
    CHECK(dir_empty(settings.temp_dir));
}

void test_redirect(const std::string& dir, testutil::TestHttpServer& server) {
    const Settings settings = test_settings(dir);
    DownloadEngine engine(settings);
    DownloadRequest req;
    req.url = server.base_url() + "/redirect/1048576";
    const auto result = engine.run(req);
    CHECK(result.status == DownloadResult::Status::Completed);
    CHECK_EQ(result.size, uint64_t(1048576));
    CHECK(file_matches(result.path, 1048576));
}

void test_forbidden(const std::string& dir, testutil::TestHttpServer& server) {
    const Settings settings = test_settings(dir);
    DownloadEngine engine(settings);
    DownloadRequest req;
    req.url = server.base_url() + "/forbidden";
    const auto result = engine.run(req);
    CHECK(result.status == DownloadResult::Status::Failed);
    CHECK(result.error.find("403") != std::string::npos);
}

void test_chunked_unknown_size(const std::string& dir, testutil::TestHttpServer& server) {
    const Settings settings = test_settings(dir);
    DownloadEngine engine(settings);
    DownloadRequest req;
    req.url = server.base_url() + "/chunked/300000";
    uint64_t reported_total = 0;
    const auto result = engine.run(req, [&](const ProgressInfo& info) {
        reported_total = info.total;
        return true;
    });
    CHECK(result.status == DownloadResult::Status::Completed);
    CHECK_EQ(result.size, uint64_t(300000));
    // Unknown-size streaming does not report a total.
    CHECK_EQ(reported_total, uint64_t(0));
    CHECK(file_matches(result.path, 300000));
}

void test_categories() {
    CHECK_EQ(category_folder_for("movie.mkv"), std::string("Video"));
    CHECK_EQ(category_folder_for("song.mp3"), std::string("Audio"));
    CHECK_EQ(category_folder_for("archive.zip"), std::string("Compressed"));
    CHECK_EQ(category_folder_for("report.PDF"), std::string("Document"));
    CHECK_EQ(category_folder_for("setup.exe"), std::string("Application"));
    CHECK_EQ(category_folder_for("mystery"), std::string("Misc"));
    CHECK_EQ(category_folder_for("data.unknownext"), std::string("Misc"));
}

void test_unique_path(const std::string& dir) {
    const std::string base = dir + "/unique.bin";
    {
        std::ofstream out(base);
        out << "x";
    }
    CHECK_EQ(unique_path(base), dir + "/unique (1).bin");
    {
        std::ofstream out(dir + "/unique (1).bin");
        out << "x";
    }
    CHECK_EQ(unique_path(base), dir + "/unique (2).bin");
}

}  // namespace

int main() {
    const std::string dir = testutil::make_temp_dir("http");
    std::filesystem::create_directories(dir);

    testutil::TestHttpServer server;
    if (!server.start()) {
        std::fprintf(stderr, "cannot start test HTTP server\n");
        return 1;
    }

    test_probe(server);
    test_browser_cookie_scope(dir + "/browser-cookie", server);
    test_download_segmented(dir + "/segmented", server);
    test_download_single_connection(dir + "/single", server);
    test_resume_after_pause(dir + "/resume", server);
    test_redirect(dir + "/redirect", server);
    test_forbidden(dir + "/forbidden", server);
    test_chunked_unknown_size(dir + "/chunked", server);
    test_categories();
    test_unique_path(dir);

    server.stop();
    testutil::cleanup_dir(dir);
    return testutil::finish("test_http");
}
