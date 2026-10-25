#include <fcntl.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "engine/categories.h"
#include "engine/download_engine.h"
#include "engine/segment_plan.h"
#include "core/util.h"
#include "http_test_server.h"
#include "net/http.h"
#include "net/interstitial.h"
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

void test_engine_reuses_confirmation_probe(const std::string& dir,
                                           testutil::TestHttpServer& server) {
    const Settings settings = test_settings(dir);
    DownloadEngine engine(settings);
    const std::string url = server.base_url() + "/file/1048576";
    HttpOptions options;
    options.url = url;
    const ProbeResult probe = probe_url(options);
    CHECK(probe.ok);
    const int heads_after_confirmation = server.head_requests();

    DownloadRequest request;
    request.url = url;
    request.probe = probe;
    const DownloadResult result = engine.run(request);
    CHECK(result.status == DownloadResult::Status::Completed);
    CHECK_EQ(server.head_requests(), heads_after_confirmation);
    CHECK_EQ(result.size, uint64_t(1048576));
    CHECK(file_matches(result.path, 1048576));
}

void test_url_encoding(testutil::TestHttpServer& server) {
    CHECK_EQ(encode_url_for_request("https://host/Test SwiftDM.zip?x=1"),
             std::string("https://host/Test%20SwiftDM.zip?x=1"));
    CHECK_EQ(encode_url_for_request("https://host/a%20b"),
             std::string("https://host/a%20b"));
    CHECK_EQ(encode_url_for_request("https://host/a%zzb"),
             std::string("https://host/a%25zzb"));
    CHECK_EQ(encode_url_for_request("https://host/caf\xC3\xA9.zip"),
             std::string("https://host/caf%C3%A9.zip"));
    CHECK_EQ(encode_url_for_request("https://host/a+b"),
             std::string("https://host/a+b"));
    CHECK_EQ(file_name_from_url("https://host/Test%20SwiftDM.zip?token=x"),
             std::string("Test SwiftDM.zip"));

    HttpOptions invalid;
    invalid.url = "drive.google.com/file/d/example";
    const ProbeResult invalid_probe = probe_url(invalid);
    CHECK(!invalid_probe.ok);
    CHECK_EQ(invalid_probe.error, std::string(kInvalidUrlError));

    const std::string raw_url = server.base_url() + "/space-file/Test SwiftDM.zip";
    HttpOptions options;
    options.url = raw_url;
    const ProbeResult probe = probe_url(options);
    CHECK(probe.ok);
    CHECK_EQ(probe.content_length, uint64_t(65536));
    CHECK(probe.download_url.find("Test%20SwiftDM.zip") != std::string::npos);
    CHECK(probe.download_url.find(' ') == std::string::npos);

    const std::string dir = testutil::make_temp_dir("space-url");
    const Settings settings = test_settings(dir);
    DownloadEngine engine(settings);
    DownloadRequest request;
    request.url = raw_url;
    const auto result = engine.run(request);
    CHECK(result.status == DownloadResult::Status::Completed);
    CHECK_EQ(result.size, uint64_t(65536));
    CHECK(file_matches(result.path, 65536));
    testutil::cleanup_dir(dir);
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

void test_sensitive_headers_do_not_cross_redirect_origin(
    const std::string& dir, testutil::TestHttpServer& server) {
    const std::string redirect_url =
        server.base_url() + "/redirect-cross-origin/65536";
    const int probe_target_before = testutil::g_cross_origin_redirect_targets.load();
    const int forwarded_headers_before = testutil::g_forwarded_sensitive_headers.load();
    const int forwarded_referers_before = testutil::g_forwarded_referers.load();

    for (const bool use_referer : {false, true}) {
        const int redirect_before = testutil::g_cross_origin_redirects.load();
        HttpOptions options;
        options.url = redirect_url;
        if (use_referer) {
            options.referer = "https://private.example/account";
        } else {
            options.headers.push_back({"x-audit-token", "secret-token"});
        }
        const ProbeResult probe = probe_url(options);
        CHECK(!probe.ok);
        CHECK_EQ(probe.error, std::string(kRedirectWithSensitiveHeadersError));
        CHECK(testutil::g_cross_origin_redirects.load() > redirect_before);
        CHECK_EQ(testutil::g_cross_origin_redirect_targets.load(), probe_target_before);
        CHECK_EQ(testutil::g_forwarded_sensitive_headers.load(), forwarded_headers_before);
        CHECK_EQ(testutil::g_forwarded_referers.load(), forwarded_referers_before);
    }

    // Exercise the transfer path independently of probe_url(): callers may
    // reuse a previously obtained ProbeResult, but the transport must still
    // stop before forwarding sensitive request metadata to another origin.
    const Settings settings = test_settings(dir);
    DownloadEngine engine(settings);
    for (const bool use_referer : {false, true}) {
        for (const bool segmented : {false, true}) {
            const int redirect_before = testutil::g_cross_origin_redirects.load();
            DownloadRequest request;
            request.url = redirect_url;
            request.output_dir = dir + (use_referer ? "/referer" : "/header") +
                                 (segmented ? "/segmented" : "/stream");
            request.apply_category_folder = false;
            if (use_referer) {
                request.referer = "https://private.example/account";
            } else {
                request.headers.push_back({"x-audit-token", "secret-token"});
            }
            ProbeResult cached_probe;
            cached_probe.ok = true;
            cached_probe.effective_url = redirect_url;
            cached_probe.mime_type = "application/octet-stream";
            cached_probe.content_length = segmented ? 65536 : 0;
            cached_probe.accept_ranges = segmented;
            request.probe = cached_probe;

            const auto result = engine.run(request);
            CHECK(result.status == DownloadResult::Status::Failed);
            CHECK_EQ(result.error, std::string(kRedirectWithSensitiveHeadersError));
            CHECK(testutil::g_cross_origin_redirects.load() > redirect_before);
            CHECK_EQ(testutil::g_cross_origin_redirect_targets.load(), probe_target_before);
            CHECK_EQ(testutil::g_forwarded_sensitive_headers.load(), forwarded_headers_before);
            CHECK_EQ(testutil::g_forwarded_referers.load(), forwarded_referers_before);
        }
    }

    // Ordinary same-origin redirects keep working with caller-supplied headers.
    HttpOptions same_origin;
    same_origin.url = server.base_url() + "/redirect/1024";
    same_origin.headers.push_back({"x-audit-token", "same-origin-token"});
    const ProbeResult allowed = probe_url(same_origin);
    CHECK(allowed.ok);

    DownloadRequest same_origin_request;
    same_origin_request.url = same_origin.url;
    same_origin_request.output_dir = dir + "/same-origin";
    same_origin_request.apply_category_folder = false;
    same_origin_request.headers = same_origin.headers;
    const auto same_origin_result = engine.run(same_origin_request);
    CHECK(same_origin_result.status == DownloadResult::Status::Completed);
    CHECK(file_matches(same_origin_result.path, 1024));
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

// Hosts that explain a refusal in the response body (pixeldrain's hotlink
// protection, connection limits, ...) must show that reason, not just the
// status line.
void test_probe_error_detail(testutil::TestHttpServer& server) {
    HttpOptions opts;
    opts.url = server.base_url() + "/json-error";
    opts.user_agent = "SwiftDM-Test/1.0";
    const ProbeResult probe = probe_url(opts);
        CHECK(!probe.ok);
    CHECK_EQ(probe.status_code, long(403));
    CHECK(probe.error.find("403 Forbidden") != std::string::npos);
    CHECK(probe.error.find("hotlink_detected") != std::string::npos);
    CHECK(probe.error.find("Hotlinking was detected") != std::string::npos);
}

void test_transfer_error_detail(const std::string& dir, testutil::TestHttpServer& server) {
    const Settings settings = test_settings(dir);
    DownloadEngine engine(settings);
    DownloadRequest req;
    req.url = server.base_url() + "/segments-forbidden/65536";
    req.connections = 4;

    const auto result = engine.run(req);
    CHECK(result.status == DownloadResult::Status::Failed);
    CHECK(result.error.find("403 Forbidden") != std::string::npos);
    CHECK(result.error.find("max_concurrent_downloads") != std::string::npos);
    CHECK(result.error.find("maximum number of open download connections") != std::string::npos);

    // The refused body must not be written into the partial file.
    CHECK(!result.path.empty());
    std::ifstream in(result.path, std::ios::binary);
    CHECK(static_cast<bool>(in));
    std::string head(64, '\0');
    in.read(&head[0], static_cast<std::streamsize>(head.size()));
    bool all_zero = true;
    for (char c : head) {
        if (c != '\0') all_zero = false;
    }
    CHECK(all_zero);
}

void test_stream_error_detail(const std::string& dir, testutil::TestHttpServer& server) {
    const Settings settings = test_settings(dir);
    DownloadEngine engine(settings);
    DownloadRequest req;
    req.url = server.base_url() + "/stream-forbidden";

    const auto result = engine.run(req);
    CHECK(result.status == DownloadResult::Status::Failed);
    CHECK(result.error.find("403 Forbidden") != std::string::npos);
    CHECK(result.error.find("max_concurrent_downloads") != std::string::npos);
}

// A host that refuses parallel ranges must still be usable: the engine falls
// back to a single connection and finishes the download.
void test_single_connection_fallback(const std::string& dir,
                                     testutil::TestHttpServer& server) {
    const Settings settings = test_settings(dir);
    DownloadEngine engine(settings);
    server.reset_limited_refusals();

    // Segments are never smaller than 1 MiB, so the parallel plan needs a file
    // of at least 4 MiB to produce four ranged requests.
    constexpr uint64_t kFallbackSize = 8u << 20;
    DownloadRequest req;
    req.url = server.base_url() + "/limit-connections/" + std::to_string(kFallbackSize);
    req.connections = 4;

    const auto result = engine.run(req);
    CHECK(result.status == DownloadResult::Status::Completed);
    CHECK_EQ(result.size, kFallbackSize);
    CHECK(file_matches(result.path, kFallbackSize));
    CHECK(server.limited_refusals() > 0);  // the parallel attempt really was refused
    CHECK(dir_empty(settings.temp_dir));
}

// HTML error pages (Cloudflare style) are summarized without their markup.
void test_html_error_detail(testutil::TestHttpServer& server) {
    HttpOptions opts;
    opts.url = server.base_url() + "/html-error";
    const ProbeResult probe = probe_url(opts);
    CHECK(!probe.ok);
    CHECK(probe.error.find("403") != std::string::npos);
    CHECK(probe.error.find("Sorry, you have been blocked") != std::string::npos);
    CHECK(probe.error.find("<html") == std::string::npos);
    CHECK(probe.error.find("var a = 1") == std::string::npos);

    // Without a <title> the visible text is used, minus scripts/styles.
    HttpOptions plain;
    plain.url = server.base_url() + "/html-error-plain";
    const ProbeResult plain_probe = probe_url(plain);
    CHECK(!plain_probe.ok);
    CHECK(plain_probe.error.find("Request blocked by the host") != std::string::npos);
    CHECK(plain_probe.error.find("var a = 1") == std::string::npos);
}

// ---------------------------------------------------------------------------
// Download interstitials (Google Drive), docs/spec/04-protocols.md §5
// ---------------------------------------------------------------------------

void test_drive_link_helpers() {
    CHECK_EQ(google_drive_file_id("https://drive.google.com/file/d/1AbC-def_123/view?usp=sharing"),
             std::string("1AbC-def_123"));
    CHECK_EQ(google_drive_file_id("https://drive.google.com/uc?export=download&id=1AbCdef123"),
             std::string("1AbCdef123"));
    CHECK_EQ(google_drive_file_id("https://drive.google.com/open?id=1AbCdef123"),
             std::string("1AbCdef123"));
    CHECK_EQ(google_drive_file_id("https://drive.google.com/drive/u/1/file/d/1AbCdef123/view"),
             std::string("1AbCdef123"));
    CHECK_EQ(google_drive_file_id(
                 "https://drive.usercontent.google.com/download?id=1AbCdef123&export=download"),
             std::string("1AbCdef123"));
    // Other hosts are left alone, even when the path looks the same.
    CHECK_EQ(google_drive_file_id("https://example.com/file/d/1AbCdef123/view"), std::string());
    CHECK(!is_google_drive_host("https://example.com/file/d/1AbCdef123/view"));
    CHECK(is_google_docs_host("https://docs.google.com/document/d/1AbCdef123/edit"));

    CHECK_EQ(google_drive_folder_id("https://drive.google.com/drive/folders/1Folder_9?usp=sharing"),
             std::string("1Folder_9"));
    CHECK_EQ(google_drive_folder_id("https://drive.google.com/drive/u/0/folders/1Folder_9"),
             std::string("1Folder_9"));
    CHECK_EQ(google_drive_folder_id("https://drive.google.com/file/d/1AbCdef123/view"),
             std::string());

    CHECK_EQ(google_drive_download_url("1AbCdef123"),
             std::string("https://drive.usercontent.google.com/download?id=1AbCdef123"
                         "&export=download"));
    CHECK_EQ(google_drive_download_url("bad id!"), std::string());
    CHECK_EQ(keep_drive_handoff_params(google_drive_download_url("1AbCdef123"),
                                       "https://drive.google.com/file/d/1AbCdef123/view"
                                       "?usp=sharing&authuser=2&resourcekey=0-Ab_c-d"),
             std::string("https://drive.usercontent.google.com/download?id=1AbCdef123"
                         "&export=download&authuser=2&resourcekey=0-Ab_c-d"));
    // A browser's own download URL already carries the confirmation flag and a
    // session uuid: the flag is kept, the uuid is not (it changes per page).
    CHECK_EQ(keep_drive_handoff_params(google_drive_download_url("1AbCdef123"),
                                       "https://drive.usercontent.google.com/download"
                                       "?id=1AbCdef123&export=download&authuser=0"
                                       "&confirm=t&uuid=6aa864b8-8504-4290-aa3b-x"),
             std::string("https://drive.usercontent.google.com/download?id=1AbCdef123"
                         "&export=download&authuser=0&confirm=t"));
    CHECK_EQ(keep_drive_handoff_params("https://host/x?authuser=5",
                                       "https://drive.google.com/x?authuser=2"),
             std::string("https://host/x?authuser=5"));
    CHECK_EQ(url_query_param("https://drive.google.com/uc?id=1AbC&authuser=3", "authuser"),
             std::string("3"));
    CHECK_EQ(url_query_param("https://drive.google.com/uc?id=1AbC", "authuser"), std::string());

    // Pages that never are the file: reported instead of saved.
    CHECK_EQ(page_error_reason("https://drive.google.com/file/d/1AbCdef123/view",
                               "https://drive.google.com/file/d/1AbCdef123/view"),
             std::string(kDrivePageError));
    CHECK_EQ(page_error_reason("https://docs.google.com/document/d/1AbCdef123/edit",
                               "https://docs.google.com/document/d/1AbCdef123/edit"),
             std::string(kDocsEditorError));
    CHECK(page_error_reason("https://example.com/download/page", "").empty());

    const std::string signin_html =
        "<html><head><title>Google Drive: Sign-in</title></head><body>"
        "<form action=\"https://accounts.google.com/v3/signin/identifier\">"
        "<input id=\"identifierId\"/></form></body></html>";
    CHECK_EQ(signin_error_reason("https://accounts.google.com/v3/signin/identifier", ""),
             std::string(kSignInError));
    CHECK_EQ(signin_error_reason("http://127.0.0.1:1/x", signin_html), std::string(kSignInError));
    CHECK(signin_error_reason("http://127.0.0.1:1/x", "<html><body>hello</body></html>").empty());
}

void test_drive_folder_link_probe() {
    // No request is made: a folder link has no file behind it.
    HttpOptions opts;
    opts.url = "https://drive.google.com/drive/folders/1AbCdef123?usp=sharing";
    const ProbeResult probe = probe_url(opts);
    CHECK(!probe.ok);
    CHECK_EQ(probe.error, std::string(kDriveFolderError));
}

void test_download_confirmation_url() {
    const std::string page =
        "<html><head><title>Google Drive - Virus scan warning</title></head><body>"
        "<form id=\"download-form\" action=\"https://drive.usercontent.google.com/download\" "
        "method=\"get\"><input type=\"submit\" value=\"Download anyway\"/>"
        "<input type=\"hidden\" name=\"id\" value=\"1AbCdef123\"/>"
        "<input type=\"hidden\" name=\"export\" value=\"download\"/>"
        "<input type=\"hidden\" name=\"confirm\" value=\"t\"/>"
        "<input type=\"hidden\" name=\"uuid\" value=\"aaaa-bbbb\"/></form></body></html>";
    CHECK_EQ(download_confirmation_url(page),
             std::string("https://drive.usercontent.google.com/download?id=1AbCdef123"
                         "&export=download&confirm=t"));
    // A form without a confirmation field is an ordinary page, not a notice.
    CHECK_EQ(download_confirmation_url("<html><body><form action=\"/x\">"
                                       "<input type=\"hidden\" name=\"id\" value=\"1\"/>"
                                       "</form></body></html>"),
             std::string());
    CHECK_EQ(download_confirmation_url("<html><body>no form here</body></html>"),
             std::string());
}

void test_drive_interstitial_probe(testutil::TestHttpServer& server) {
    HttpOptions opts;
    opts.url = server.base_url() + "/drive-page/262144";
    opts.user_agent = "SwiftDM-Test/1.0";
    const ProbeResult probe = probe_url(opts);
    CHECK(probe.ok);
    CHECK_EQ(probe.content_length, uint64_t(262144));
    CHECK(probe.accept_ranges);
    CHECK_EQ(probe.filename_hint, std::string("sample-262144.bin"));
    CHECK(probe.mime_type.find("html") == std::string::npos);
    CHECK(probe.download_url.find("confirm=t") != std::string::npos);
    CHECK(probe.download_url.find("uuid=") == std::string::npos);  // stable across runs
    CHECK_EQ(probe.effective_url, probe.download_url);
}

void test_drive_interstitial_download(const std::string& dir, testutil::TestHttpServer& server) {
    const Settings settings = test_settings(dir);
    DownloadEngine engine(settings);
    DownloadRequest req;
    req.url = server.base_url() + "/drive-page/1048576";
    const auto result = engine.run(req);
    CHECK(result.status == DownloadResult::Status::Completed);
    CHECK(result.path.find("sample-1048576.bin") != std::string::npos);
    CHECK(file_matches(result.path, 1048576));
    CHECK(dir_empty(settings.temp_dir));
}

void test_drive_page_unresolved(testutil::TestHttpServer& server) {
    HttpOptions opts;
    opts.url = server.base_url() + "/drive-loop/65536";
    const ProbeResult probe = probe_url(opts);
    CHECK(!probe.ok);
    CHECK(probe.error.find("confirmation page") != std::string::npos);
}

void test_signin_page(testutil::TestHttpServer& server) {
    HttpOptions opts;
    opts.url = server.base_url() + "/signin";
    const ProbeResult probe = probe_url(opts);
    CHECK(!probe.ok);
    CHECK_EQ(probe.error, std::string(kSignInError));
}

// A plain HTML page from an ordinary host is still downloadable as a file.
void test_plain_page_download(const std::string& dir, testutil::TestHttpServer& server) {
    const Settings settings = test_settings(dir);
    DownloadEngine engine(settings);
    DownloadRequest req;
    req.url = server.base_url() + "/page";
    const auto result = engine.run(req);
    CHECK(result.status == DownloadResult::Status::Completed);
    CHECK(result.path.find("/page") != std::string::npos);
    std::ifstream in(result.path, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    CHECK(text.find("<title>Plain page</title>") != std::string::npos);
}

// A page may only steer the transparent confirmation step to its own origin
// (audit finding 2). The fixture's form points at `localhost` while the page is
// served from `127.0.0.1`, so following it would prove the vulnerability.
void test_confirmation_form_cross_origin(const std::string& dir,
                                         testutil::TestHttpServer& server) {
    const int before = testutil::g_confirm_form_requests.load();
    HttpOptions opts;
    opts.url = server.base_url() + "/confirm-form-page";
    const ProbeResult probe = probe_url(opts);
    CHECK(!probe.ok);
    CHECK_EQ(probe.error, std::string(kConfirmationPendingError));
    CHECK_EQ(testutil::g_confirm_form_requests.load(), before);

    const Settings settings = test_settings(dir);
    DownloadEngine engine(settings);
    DownloadRequest req;
    req.url = server.base_url() + "/confirm-form-page";
    const auto result = engine.run(req);
    CHECK(result.status == DownloadResult::Status::Failed);
    CHECK(result.error.find("confirmation page") != std::string::npos);
    CHECK_EQ(testutil::g_confirm_form_requests.load(), before);
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

// The speed limiter toggle is authoritative: on applies the value to the
// sequential (unknown-size) path as well, off means unlimited no matter what
// value is stored per download or in the settings. libcurl enforces the rate
// on the wall clock, so only lower bounds are asserted: 256 KiB at 128 KiB/s
// cannot finish in well under two seconds.
void test_bandwidth_limit_toggle(const std::string& dir, testutil::TestHttpServer& server) {
    Settings settings = test_settings(dir);
    settings.bandwidth_limit_enabled = true;
    settings.bandwidth_limit_kbps = 8;  // applied by mistake: needs ~32 s
    DownloadEngine engine(settings);

    constexpr uint64_t kSize = 256u << 10;
    auto run_seconds = [&](const DownloadRequest& req) {
        const auto start = std::chrono::steady_clock::now();
        const auto result = engine.run(req);
        const double seconds = std::chrono::duration<double>(
                                   std::chrono::steady_clock::now() - start)
                                   .count();
        CHECK(result.status == DownloadResult::Status::Completed);
        CHECK_EQ(result.size, kSize);
        CHECK(file_matches(result.path, kSize));
        return seconds;
    };

    DownloadRequest limited;
    limited.url = server.base_url() + "/chunked/" + std::to_string(kSize);
    limited.bandwidth_limit_enabled = true;
    limited.bandwidth_limit_kbps = 128;  // >= ~2 s
    CHECK(run_seconds(limited) >= 1.5);

    DownloadRequest disabled = limited;
    disabled.bandwidth_limit_enabled = false;
    CHECK(run_seconds(disabled) < 4.0);

    // No fallback to the global setting while the toggle is off.
    disabled.bandwidth_limit_kbps = 0;
    CHECK(run_seconds(disabled) < 4.0);
}

void test_live_bandwidth_limit(const std::string& dir, testutil::TestHttpServer& server) {
    Settings settings = test_settings(dir);
    DownloadEngine engine(settings);
    constexpr uint64_t kSize = 4u << 20;
    const auto run_and_raise_limit = [&](const std::string& url, int new_limit) {
        DownloadRequest request;
        request.url = url;
        request.connections = 4;
        request.bandwidth_limit_enabled = true;
        request.bandwidth_limit_kbps = 32;
        request.live_bandwidth_limit = std::make_shared<BandwidthLimit>(32 * 1024);

        const auto start = std::chrono::steady_clock::now();
        // The test thread represents the independent GUI thread. Wait long
        // enough for the fast local probe and first transfer bytes, then edit
        // the atomic while the worker remains inside its active download.
        std::thread editor([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            request.live_bandwidth_limit->set_bytes_per_second(new_limit);
        });
        const auto result = engine.run(request);
        editor.join();
        const double seconds = std::chrono::duration<double>(
                                   std::chrono::steady_clock::now() - start)
                                   .count();
        CHECK(result.status == DownloadResult::Status::Completed);
        CHECK_EQ(result.size, kSize);
        CHECK(file_matches(result.path, kSize));
        CHECK(seconds < 20.0);
    };

    // Ranged/multi-handle transfer and sequential chunked transfer both use
    // the same live control while running.
    run_and_raise_limit(server.base_url() + "/file/" + std::to_string(kSize), 1024 * 1024);
    run_and_raise_limit(server.base_url() + "/chunked/" + std::to_string(kSize), 0);
}

void test_exact_folder_override(const std::string& dir, testutil::TestHttpServer& server) {
    Settings settings = test_settings(dir);
    settings.category_folders = true;
    settings.max_connections = 2;
    DownloadEngine engine(settings);

    // The confirmation dialog computes the category folder itself and passes
    // the exact folder; the engine must not append the extension-derived one.
    DownloadRequest exact;
    exact.url = server.base_url() + "/file/65536";
    exact.output_dir = dir + "/exact";
    exact.apply_category_folder = false;
    const auto exact_result = engine.run(exact);
    CHECK(exact_result.status == DownloadResult::Status::Completed);
    CHECK(exact_result.path.find("/exact/sample-65536.bin") != std::string::npos);
    CHECK(exact_result.path.find("/Application/") == std::string::npos);
    CHECK(file_matches(exact_result.path, 65536));

    // Legacy callers (CLI, old rows) keep the appended category folder.
    DownloadRequest legacy;
    legacy.url = server.base_url() + "/file/65536";
    legacy.output_dir = dir + "/legacy";
    const auto legacy_result = engine.run(legacy);
    CHECK(legacy_result.status == DownloadResult::Status::Completed);
    CHECK(legacy_result.path.find("/legacy/Application/sample-65536.bin") !=
          std::string::npos);
}

void test_categories() {
    const std::vector<std::string>& names = category_names();
    CHECK_EQ(names.size(), size_t(6));
    CHECK_EQ(names.front(), std::string("Video"));
    CHECK_EQ(names.back(), std::string("Misc"));
    CHECK(is_category_name("Misc"));
    CHECK(!is_category_name("misc"));
    CHECK_EQ(category_folder_for("movie.mkv"), std::string("Video"));
    CHECK_EQ(category_folder_for("song.mp3"), std::string("Audio"));
    CHECK_EQ(category_folder_for("archive.zip"), std::string("Compressed"));
    CHECK_EQ(category_folder_for("report.PDF"), std::string("Document"));
    CHECK_EQ(category_folder_for("setup.exe"), std::string("Application"));
    CHECK_EQ(category_folder_for("mystery"), std::string("Misc"));
    CHECK_EQ(category_folder_for("data.unknownext"), std::string("Misc"));
}

// The scheme allowlist gates every URL before it reaches libcurl: probes, HLS
// playlist entries, range segments and sequential transfers (audit finding 1).
void test_download_url_scheme_allowlist(const std::string& dir) {
    CHECK(is_supported_download_url("http://example.com/file.bin"));
    CHECK(is_supported_download_url("https://example.com/file.bin?x=1"));
    CHECK(is_supported_download_url("ftp://example.com/file.bin"));
    CHECK(is_supported_download_url("FTPS://example.com/file.bin"));
    CHECK(!is_supported_download_url("file:///etc/passwd"));
    CHECK(!is_supported_download_url("file://localhost/etc/passwd"));
    CHECK(!is_supported_download_url("dict://example.com:2628/x"));
    CHECK(!is_supported_download_url("gopher://example.com/1"));
    CHECK(!is_supported_download_url("data:text/plain,x"));
    CHECK(!is_supported_download_url("example.com/file.bin"));
    CHECK(!is_supported_download_url("http://"));
    CHECK(!is_supported_download_url("http://bad host/file"));

    // fetch_text and stream_download refuse the scheme even when a caller
    // skips the probe (the HLS path used to do exactly that).
    HttpOptions options;
    options.url = "file:///etc/passwd";
    std::string text;
    std::string error;
    CHECK(!fetch_text(options, &text, nullptr, &error, 4096));
    CHECK_EQ(error, std::string(kInvalidUrlError));
    CHECK(text.empty());

    const std::string sink = dir + "/scheme-sink.bin";
    std::error_code ec_dir;
    std::filesystem::create_directories(dir, ec_dir);
    const int fd = ::open(sink.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    CHECK(fd >= 0);
    uint64_t written = 0;
    error.clear();
    CHECK(stream_download(options, fd, nullptr, &written, &error) ==
          TransferOutcome::Failed);
    CHECK_EQ(error, std::string(kInvalidUrlError));
    CHECK_EQ(written, uint64_t(0));
    ::close(fd);

    DownloadEngine engine(test_settings(dir));
    DownloadRequest request;
    request.url = "file:///etc/passwd";
    const auto result = engine.run(request);
    CHECK(result.status == DownloadResult::Status::Failed);
    CHECK_EQ(result.error, std::string(kInvalidUrlError));
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
    test_engine_reuses_confirmation_probe(dir + "/probe-reuse", server);
    test_url_encoding(server);
    test_browser_cookie_scope(dir + "/browser-cookie", server);
    test_download_segmented(dir + "/segmented", server);
    test_download_single_connection(dir + "/single", server);
    test_exact_folder_override(dir + "/exact-folder", server);
    test_resume_after_pause(dir + "/resume", server);
    test_redirect(dir + "/redirect", server);
    test_sensitive_headers_do_not_cross_redirect_origin(dir + "/redirect-guard", server);
    test_forbidden(dir + "/forbidden", server);
    test_probe_error_detail(server);
    test_transfer_error_detail(dir + "/transfer-error", server);
    test_stream_error_detail(dir + "/stream-error", server);
    test_single_connection_fallback(dir + "/fallback", server);
    test_html_error_detail(server);
    test_chunked_unknown_size(dir + "/chunked", server);
    test_bandwidth_limit_toggle(dir + "/bandwidth-limit", server);
    test_live_bandwidth_limit(dir + "/live-bandwidth-limit", server);
    test_drive_link_helpers();
    test_drive_folder_link_probe();
    test_download_confirmation_url();
    test_drive_interstitial_probe(server);
    test_drive_interstitial_download(dir + "/drive", server);
    test_drive_page_unresolved(server);
    test_signin_page(server);
    test_plain_page_download(dir + "/page", server);
    test_confirmation_form_cross_origin(dir + "/confirm-cross-origin", server);
    test_categories();
    test_download_url_scheme_allowlist(dir + "/scheme");
    test_unique_path(dir);

    server.stop();
    testutil::cleanup_dir(dir);
    return testutil::finish("test_http");
}
