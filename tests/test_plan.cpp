#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "engine/segment_plan.h"
#include "test_util.h"

using namespace swiftdm;

namespace {

void test_plan_basic() {
    // 10 MB over 4 connections, 1 MB min chunk -> 4 equal segments.
    const uint64_t size = 10u << 20;
    auto segs = plan_segments(size, 4);
    CHECK_EQ(segs.size(), size_t(4));
    uint64_t total = 0;
    uint64_t expect_start = 0;
    for (const auto& s : segs) {
        CHECK_EQ(s.start, expect_start);
        CHECK(s.end >= s.start);
        CHECK(s.done == 0);
        total += s.length();
        expect_start = s.end + 1;
    }
    CHECK_EQ(total, size);
    CHECK_EQ(expect_start, size);
    CHECK_EQ(segs[0].length(), size / 4);
}

void test_plan_min_chunk() {
    // 10 MB, 32 connections, 1 MB min chunk -> capped at 10 segments.
    auto segs = plan_segments(10u << 20, 32, 1u << 20);
    CHECK_EQ(segs.size(), size_t(10));

    // 500 KB file with 8 connections and 1 MB min chunk -> single segment.
    auto small = plan_segments(500u << 10, 8, 1u << 20);
    CHECK_EQ(small.size(), size_t(1));
    CHECK_EQ(small[0].start, uint64_t(0));
    CHECK_EQ(small[0].end, (500u << 10) - 1);

    // Smaller min chunk allows more segments.
    auto multi = plan_segments(10u << 20, 32, 100u << 10);
    CHECK_EQ(multi.size(), size_t(32));
}

void test_plan_edge_cases() {
    CHECK(plan_segments(0, 4).empty());
    CHECK_EQ(plan_segments(1, 4).size(), size_t(1));
    CHECK_EQ(plan_segments(1, 4)[0].start, uint64_t(0));
    CHECK_EQ(plan_segments(1, 4)[0].end, uint64_t(0));

    auto zero_conn = plan_segments(1000, 0);
    CHECK_EQ(zero_conn.size(), size_t(1));
    CHECK_EQ(zero_conn[0].length(), uint64_t(1000));

    // Remainder distribution: 10 bytes over 3 -> 4,3,3.
    auto rem = plan_segments(10, 3, 1);
    CHECK_EQ(rem.size(), size_t(3));
    CHECK_EQ(rem[0].length(), uint64_t(4));
    CHECK_EQ(rem[1].length(), uint64_t(3));
    CHECK_EQ(rem[2].length(), uint64_t(3));

    // Full coverage on an awkward size.
    auto odd = plan_segments(99991, 7, 1);
    uint64_t cov = 0;
    for (const auto& s : odd) cov += s.length();
    CHECK_EQ(cov, uint64_t(99991));
}

void test_state_accounting() {
    TransferState st;
    st.file_size = 100;
    st.segments = plan_segments(100, 2, 1);  // [0,49], [50,99]
    CHECK_EQ(st.done_bytes(), uint64_t(0));
    CHECK(!st.finished());

    st.segments[0].done = 50;  // full first segment
    CHECK_EQ(st.done_bytes(), uint64_t(50));
    CHECK(!st.finished());

    st.segments[1].done = 50;
    CHECK(st.finished());
    CHECK_EQ(st.done_bytes(), uint64_t(100));

    // Over-count safety.
    st.segments[1].done = 999;
    CHECK(st.finished());
    CHECK_EQ(st.done_bytes(), uint64_t(100));

    TransferState empty;
    empty.file_size = 0;
    CHECK(empty.finished());
}

void test_state_roundtrip(const std::string& dir) {
    const std::string path = dir + "/segments.json";
    TransferState st;
    st.url = "https://example.com/f\";ile\\name?q=1&x=\u00e9\u00f1\U0001F600";
    st.file_size = 123456789;
    st.etag = "W/\"abc-123\"";
    st.last_modified = "Wed, 21 Oct 2015 07:28:00 GMT";
    st.segments = {{0, 49999999, 12345}, {50000000, 99999999, 0}, {100000000, 123456788, 7}};

    std::string err;
    CHECK(save_transfer_state(path, st, &err));
    CHECK(err.empty());

    TransferState loaded;
    CHECK(load_transfer_state(path, loaded, &err));
    CHECK_EQ(loaded.url, st.url);
    CHECK_EQ(loaded.file_size, st.file_size);
    CHECK_EQ(loaded.etag, st.etag);
    CHECK_EQ(loaded.last_modified, st.last_modified);
    CHECK_EQ(loaded.segments.size(), st.segments.size());
    for (size_t i = 0; i < st.segments.size(); ++i) {
        CHECK_EQ(loaded.segments[i].start, st.segments[i].start);
        CHECK_EQ(loaded.segments[i].end, st.segments[i].end);
        CHECK_EQ(loaded.segments[i].done, st.segments[i].done);
    }
    CHECK_EQ(loaded.done_bytes(), st.done_bytes());
}

void test_state_unicode_escape(const std::string& dir) {
    // Writer emits raw UTF-8; parser must also decode \u escapes.
    const std::string path = dir + "/escaped.json";
    std::ofstream out(path);
    out << "{\"version\":1,\"url\":\"https://x.test/\\u00e9 \\ud83d\\ude00 done\","
        << "\"fileSize\":10,\"etag\":\"\",\"lastModified\":\"\","
        << "\"segments\":[{\"start\":0,\"end\":9,\"done\":0}]}";
    out.close();

    TransferState st;
    std::string err;
    CHECK(load_transfer_state(path, st, &err));
    CHECK_EQ(st.url, std::string("https://x.test/\u00e9 \U0001F600 done"));
    CHECK_EQ(st.file_size, uint64_t(10));
}

void test_state_backup_fallback(const std::string& dir) {
    const std::string path = dir + "/fallback.json";
    TransferState first;
    first.url = "https://first.example/";
    first.file_size = 10;
    first.segments = {{0, 9, 3}};

    TransferState second = first;
    second.url = "https://second.example/";
    second.segments[0].done = 9;

    std::string err;
    CHECK(save_transfer_state(path, first, &err));   // primary = first
    CHECK(save_transfer_state(path, second, &err));  // primary = second, bak = first

    // Corrupt primary: load must fall back to the backup (first).
    {
        std::ofstream out(path, std::ios::trunc);
        out << "{ this is not valid json";
    }
    TransferState loaded;
    CHECK(load_transfer_state(path, loaded, &err));
    CHECK_EQ(loaded.url, first.url);
    CHECK_EQ(loaded.segments[0].done, uint64_t(3));

    // Neither file present -> failure.
    remove_transfer_state(path);
    CHECK(!load_transfer_state(path, loaded, &err));
    CHECK(!err.empty());
}

void test_state_rejects_invalid(const std::string& dir) {
    const std::string path = dir + "/invalid.json";
    auto write_and_load = [&](const std::string& text, TransferState& st) {
        std::ofstream out(path, std::ios::trunc);
        out << text;
        out.close();
        std::string err;
        return load_transfer_state(path, st, &err);
    };

    TransferState st;
    CHECK(!write_and_load("not json at all", st));
    CHECK(!write_and_load("{\"version\":1,\"fileSize\":10,\"segments\":[]}", st));
    // Gap between segments.
    CHECK(!write_and_load(
        "{\"fileSize\":20,\"segments\":[{\"start\":0,\"end\":4,\"done\":0},"
        "{\"start\":6,\"end\":19,\"done\":0}]}", st));
    // done > length.
    CHECK(!write_and_load(
        "{\"fileSize\":10,\"segments\":[{\"start\":0,\"end\":9,\"done\":11}]}", st));
    // end < start.
    CHECK(!write_and_load(
        "{\"fileSize\":10,\"segments\":[{\"start\":5,\"end\":4,\"done\":0}]}", st));
}

}  // namespace

int main() {
    const std::string dir = testutil::make_temp_dir("plan");

    test_plan_basic();
    test_plan_min_chunk();
    test_plan_edge_cases();
    test_state_accounting();
    test_state_roundtrip(dir);
    test_state_unicode_escape(dir);
    test_state_backup_fallback(dir);
    test_state_rejects_invalid(dir);

    testutil::cleanup_dir(dir);
    return testutil::finish("test_plan");
}
