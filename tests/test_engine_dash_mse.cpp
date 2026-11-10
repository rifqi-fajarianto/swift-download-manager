#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "engine/download_engine.h"
#include "http_test_server.h"
#include "store/settings.h"
#include "test_util.h"

using namespace swiftdm;

namespace {

Settings test_settings(const std::string& dir) {
    Settings s = default_settings();
    s.download_dir = dir + "/out";
    s.temp_dir = dir + "/tmp";
    s.category_folders = false;
    s.max_connections = 4;
    s.default_agent = "SwiftDM-Test/1.0";
    return s;
}

std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// Mirrors the server's DASH segment body formula (64 deterministic bytes).
std::string dash_body(int rep_index, int number) {
    const uint64_t offset = static_cast<uint64_t>(rep_index) * 100000u +
                            static_cast<uint64_t>(number) * 16u;
    std::string body;
    for (int i = 0; i < 64; ++i) {
        body.push_back(static_cast<char>(testutil::content_byte(offset + static_cast<uint64_t>(i))));
    }
    return body;
}

void test_run_dash(const std::string& dir, testutil::TestHttpServer& server) {
    const Settings settings = test_settings(dir);
    DownloadEngine engine(settings);

    DownloadRequest req;
    req.url = server.base_url() + "/dash/manifest.mpd";
    // The manifest name must not stick to the output: the bytes are the
    // representation, so the engine renames .mpd to .mp4.
    req.filename = "dash.mpd";
    req.media_kind = "dash";
    req.variant_id = "v1";

    const DownloadResult result = engine.run(req);
    if (result.status != DownloadResult::Status::Completed) {
        std::fprintf(stderr, "dash error: %s\n", result.error.c_str());
    }
    CHECK(result.status == DownloadResult::Status::Completed);
    CHECK(result.path.size() > 4 &&
          result.path.compare(result.path.size() - 4, 4, ".mp4") == 0);
    // init + four segments, 64 bytes each.
    CHECK_EQ(result.size, uint64_t(320));
    std::string expected = dash_body(0, 0);
    for (int n = 1; n <= 4; ++n) expected += dash_body(0, n);
    const std::string actual = read_file(result.path);
    CHECK(actual == expected);
}

void test_run_mse(const std::string& dir, testutil::TestHttpServer& server) {
    const Settings settings = test_settings(dir);
    DownloadEngine engine(settings);

    DownloadRequest req;
    req.url = server.base_url() + "/mse/seg-0.m4s";
    req.filename = "mse.mp4";
    req.media_kind = "mse";
    for (int n = 0; n < 3; ++n) {
        ui::MediaSegment segment;
        segment.url = server.base_url() + "/mse/seg-" + std::to_string(n) + ".m4s";
        segment.role = n == 0 ? "init" : "media";
        segment.order = n;
        segment.content_type = "video/iso.segment";
        req.segments.push_back(segment);
    }

    const DownloadResult result = engine.run(req);
    if (result.status != DownloadResult::Status::Completed) {
        std::fprintf(stderr, "mse error: %s\n", result.error.c_str());
    }
    CHECK(result.status == DownloadResult::Status::Completed);
    // Each segment is moof(52) + mdat(24).
    CHECK_EQ(result.size, uint64_t(3 * 76));
    const std::string body = read_file(result.path);
    CHECK_EQ(body.size(), size_t(3 * 76));
    CHECK(body.compare(4, 4, "moof") == 0);
    CHECK(body.compare(12, 4, "mfhd") == 0);
    const uint32_t first_sequence = (static_cast<uint32_t>(static_cast<uint8_t>(body[20])) << 24) |
                                    (static_cast<uint32_t>(static_cast<uint8_t>(body[21])) << 16) |
                                    (static_cast<uint32_t>(static_cast<uint8_t>(body[22])) << 8) |
                                    static_cast<uint32_t>(static_cast<uint8_t>(body[23]));
    CHECK_EQ(first_sequence, uint32_t(0));
    // The last segment's payload is the deterministic test content for sequence 2.
    std::string last_payload;
    for (int i = 0; i < 16; ++i) {
        last_payload.push_back(static_cast<char>(testutil::content_byte(32 + static_cast<uint64_t>(i))));
    }
    CHECK(body.compare(body.size() - 16, 16, last_payload) == 0);
}

void test_run_mse_gap(const std::string& dir, testutil::TestHttpServer& server) {
    const Settings settings = test_settings(dir);
    DownloadEngine engine(settings);

    DownloadRequest req;
    req.url = server.base_url() + "/mse/gap-0.m4s";
    req.filename = "gap.mp4";
    req.media_kind = "mse";
    for (int n = 0; n < 2; ++n) {
        ui::MediaSegment segment;
        segment.url = server.base_url() + "/mse/gap-" + std::to_string(n) + ".m4s";
        segment.role = "media";
        segment.order = n;
        req.segments.push_back(segment);
    }

    const DownloadResult result = engine.run(req);
    CHECK(result.status == DownloadResult::Status::Failed);
    CHECK(result.error.find("gap") != std::string::npos);
    // The broken assembly is removed instead of being written as a file.
    CHECK(!std::filesystem::exists(result.path));
}

}  // namespace

int main() {
    const std::string dir = testutil::make_temp_dir("engine-dash-mse");
    testutil::TestHttpServer server;
    if (!server.start()) {
        std::fprintf(stderr, "cannot start test server\n");
        return 1;
    }

    test_run_dash(dir + "/dash", server);
    test_run_mse(dir + "/mse", server);
    test_run_mse_gap(dir + "/gap", server);

    server.stop();
    testutil::cleanup_dir(dir);
    return testutil::finish("test_engine_dash_mse");
}
