#include "engine/segment_plan.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include "core/util.h"
#include "core/win_headers.h"

namespace swiftdm {

uint64_t TransferState::done_bytes() const {
    uint64_t total = 0;
    for (const Segment& s : segments) {
        uint64_t d = s.done;
        if (d > s.length()) d = s.length();
        total += d;
    }
    return total;
}

bool TransferState::finished() const {
    if (file_size == 0) return true;
    if (segments.empty()) return false;
    for (const Segment& s : segments) {
        if (!s.finished()) return false;
    }
    return true;
}

std::vector<Segment> plan_segments(uint64_t file_size, int connections,
                                   uint64_t min_chunk) {
    std::vector<Segment> out;
    if (file_size == 0) return out;
    if (min_chunk == 0) min_chunk = 1;
    if (connections < 1) connections = 1;

    uint64_t max_by_chunk = (file_size + min_chunk - 1) / min_chunk;  // ceil
    uint64_t n = static_cast<uint64_t>(connections);
    if (n > max_by_chunk) n = max_by_chunk;
    if (n < 1) n = 1;

    out.reserve(n);
    const uint64_t base = file_size / n;
    const uint64_t rem = file_size % n;
    uint64_t offset = 0;
    for (uint64_t i = 0; i < n; ++i) {
        const uint64_t len = base + (i < rem ? 1u : 0u);
        Segment s;
        s.start = offset;
        s.end = offset + len - 1;
        s.done = 0;
        out.push_back(s);
        offset += len;
    }
    return out;
}

// ---------------------------------------------------------------------------
// JSON state file (see docs/spec/02-data-model.md)
// ---------------------------------------------------------------------------

namespace {

void append_u64(std::string& out, uint64_t v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(v));
    out += buf;
}

void append_json_string(std::string& out, const std::string& s) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    out += '"';
}

std::string serialize_state(const TransferState& st) {
    std::string out;
    out.reserve(256 + st.segments.size() * 48);
    out += "{\"version\":";
    append_u64(out, static_cast<uint64_t>(st.version));
    out += ",\"url\":";
    append_json_string(out, st.url);
    out += ",\"fileSize\":";
    append_u64(out, st.file_size);
    out += ",\"etag\":";
    append_json_string(out, st.etag);
    out += ",\"lastModified\":";
    append_json_string(out, st.last_modified);
    out += ",\"segments\":[";
    for (size_t i = 0; i < st.segments.size(); ++i) {
        if (i) out += ',';
        const Segment& s = st.segments[i];
        out += "{\"start\":";
        append_u64(out, s.start);
        out += ",\"end\":";
        append_u64(out, s.end);
        out += ",\"done\":";
        append_u64(out, s.done);
        out += '}';
    }
    out += "]}\n";
    return out;
}

// Minimal JSON reader for the fixed state schema above. Tolerant to unknown
// keys (their values are skipped); strictly validates what it does read.
class StateJsonParser {
  public:
    explicit StateJsonParser(const std::string& text) : s_(text) {}

    bool parse(TransferState& out) {
        out = TransferState{};
        if (!expect('{')) return false;
        bool first = true;
        while (true) {
            skip_ws();
            if (peek() == '}') { ++pos_; break; }
            if (!first && !expect(',')) return false;
            first = false;
            std::string key;
            if (!parse_string(key)) return false;
            if (!expect(':')) return false;
            if (key == "version") {
                int64_t v = 0;
                if (!parse_int64(v)) return false;
                out.version = static_cast<int>(v);
            } else if (key == "url") {
                if (!parse_string(out.url)) return false;
            } else if (key == "fileSize") {
                if (!parse_uint64(out.file_size)) return false;
            } else if (key == "etag") {
                if (!parse_string(out.etag)) return false;
            } else if (key == "lastModified") {
                if (!parse_string(out.last_modified)) return false;
            } else if (key == "segments") {
                if (!parse_segments(out.segments)) return false;
            } else {
                if (!skip_value()) return false;
            }
        }
        return ok_ && validate(out);
    }

  private:
    const std::string& s_;
    size_t pos_ = 0;
    bool ok_ = true;

    char peek() { return pos_ < s_.size() ? s_[pos_] : '\0'; }
    bool expect(char c) {
        skip_ws();
        if (peek() != c) { ok_ = false; return false; }
        ++pos_;
        return true;
    }
    void skip_ws() {
        while (pos_ < s_.size()) {
            const char c = s_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++pos_;
            else break;
        }
    }
    static void append_utf8(std::string& out, unsigned cp) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    bool parse_hex4(unsigned& out) {
        if (pos_ + 4 > s_.size()) return false;
        unsigned v = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = s_[pos_ + i];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<unsigned>(c - 'A' + 10);
            else return false;
        }
        out = v;
        pos_ += 4;
        return true;
    }
    bool parse_string(std::string& out) {
        skip_ws();
        if (peek() != '"') { ok_ = false; return false; }
        ++pos_;
        out.clear();
        while (pos_ < s_.size()) {
            const unsigned char c = static_cast<unsigned char>(s_[pos_++]);
            if (c == '"') return true;
            if (c != '\\') {
                out += static_cast<char>(c);
                continue;
            }
            if (pos_ >= s_.size()) break;
            const char esc = s_[pos_++];
            switch (esc) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    unsigned cp = 0;
                    if (!parse_hex4(cp)) { ok_ = false; return false; }
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        // Expect a low surrogate.
                        if (pos_ + 1 < s_.size() && s_[pos_] == '\\' && s_[pos_ + 1] == 'u') {
                            pos_ += 2;
                            unsigned lo = 0;
                            if (!parse_hex4(lo)) { ok_ = false; return false; }
                            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            }
                        } else {
                            ok_ = false;
                            return false;
                        }
                    }
                    append_utf8(out, cp);
                    break;
                }
                default: ok_ = false; return false;
            }
        }
        ok_ = false;
        return false;
    }
    bool parse_uint64(uint64_t& out) {
        skip_ws();
        const size_t begin = pos_;
        while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
        if (pos_ == begin) { ok_ = false; return false; }
        errno = 0;
        const std::string digits = s_.substr(begin, pos_ - begin);
        out = std::strtoull(digits.c_str(), nullptr, 10);
        if (errno == ERANGE) { ok_ = false; return false; }
        return true;
    }
    bool parse_int64(int64_t& out) {
        skip_ws();
        const size_t begin = pos_;
        if (peek() == '-') ++pos_;
        while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
        if (pos_ == begin || (pos_ == begin + 1 && s_[begin] == '-')) {
            ok_ = false;
            return false;
        }
        out = std::strtoll(s_.substr(begin, pos_ - begin).c_str(), nullptr, 10);
        return true;
    }
    bool skip_value() {
        skip_ws();
        const char c = peek();
        if (c == '"') {
            std::string tmp;
            return parse_string(tmp);
        }
        if (c == '{' || c == '[') {
            const char close = (c == '{') ? '}' : ']';
            ++pos_;
            bool first = true;
            while (true) {
                skip_ws();
                if (peek() == close) { ++pos_; return true; }
                if (!first && !expect(',')) return false;
                first = false;
                if (close == '}') {
                    std::string key;
                    if (!parse_string(key)) return false;
                    if (!expect(':')) return false;
                }
                if (!skip_value()) return false;
            }
        }
        // number / true / false / null: consume token characters
        const size_t begin = pos_;
        while (pos_ < s_.size()) {
            const char t = s_[pos_];
            if (t == ',' || t == '}' || t == ']' || t == ' ' || t == '\t' ||
                t == '\n' || t == '\r') break;
            ++pos_;
        }
        if (pos_ == begin) { ok_ = false; return false; }
        return true;
    }
    bool parse_segments(std::vector<Segment>& out) {
        if (!expect('[')) return false;
        out.clear();
        bool first = true;
        while (true) {
            skip_ws();
            if (peek() == ']') { ++pos_; return true; }
            if (!first && !expect(',')) return false;
            first = false;
            Segment seg;
            bool have_start = false, have_end = false, have_done = false;
            if (!expect('{')) return false;
            bool sfirst = true;
            while (true) {
                skip_ws();
                if (peek() == '}') { ++pos_; break; }
                if (!sfirst && !expect(',')) return false;
                sfirst = false;
                std::string key;
                if (!parse_string(key)) return false;
                if (!expect(':')) return false;
                if (key == "start") { if (!parse_uint64(seg.start)) return false; have_start = true; }
                else if (key == "end") { if (!parse_uint64(seg.end)) return false; have_end = true; }
                else if (key == "done") { if (!parse_uint64(seg.done)) return false; have_done = true; }
                else if (!skip_value()) return false;
            }
            if (!have_start || !have_end) { ok_ = false; return false; }
            (void)have_done;  // done defaults to 0
            out.push_back(seg);
        }
    }
    static bool validate(const TransferState& st) {
        if (st.file_size == 0 && st.segments.empty()) return true;
        if (st.segments.empty()) return false;
        uint64_t expect_start = 0;
        for (const Segment& s : st.segments) {
            if (s.end < s.start) return false;
            if (s.start != expect_start) return false;
            if (s.done > s.length()) return false;
            expect_start = s.end + 1;
        }
        if (st.file_size != 0 && expect_start != st.file_size) return false;
        return true;
    }
};

bool read_file(const std::string& path, std::string& out, std::string* error) {
    std::ifstream in(path_from_utf8(path), std::ios::binary);
    if (!in) {
        if (error) *error = "cannot open '" + path + "'";
        return false;
    }
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    if (size < 0 || size > (64 << 20)) {
        if (error) *error = "state file unreadable or too large";
        return false;
    }
    in.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(size));
    if (size > 0) in.read(out.data(), size);
    if (!in) {
        if (error) *error = "read failed for '" + path + "'";
        return false;
    }
    return true;
}

bool load_from(const std::string& path, TransferState& out, std::string* error) {
    std::string text;
    if (!read_file(path, text, error)) return false;
    StateJsonParser parser(text);
    if (!parser.parse(out)) {
        if (error) *error = "invalid state in '" + path + "'";
        return false;
    }
    return true;
}

}  // namespace

std::string backup_path(const std::string& path) { return path + ".bak"; }

bool save_transfer_state(const std::string& path, const TransferState& state,
                         std::string* error) {
    const std::string text = serialize_state(state);
    const std::string tmp = path + ".tmp";

    {
#ifdef _WIN32
        FILE* f = nullptr;
        const auto wide_tmp = path_from_utf8(tmp).native();
        if (_wfopen_s(&f, wide_tmp.c_str(), L"wb") != 0) f = nullptr;
#else
        FILE* f = std::fopen(tmp.c_str(), "wb");
#endif
        if (!f) {
            if (error) *error = "cannot open '" + tmp + "': " + errno_string(errno);
            return false;
        }
        const size_t written = std::fwrite(text.data(), 1, text.size(), f);
        const int flush_rc = std::fflush(f);
        if (written != text.size() || flush_rc != 0) {
            std::fclose(f);
            std::filesystem::remove(path_from_utf8(tmp));
            if (error) *error = "write failed for '" + tmp + "'";
            return false;
        }
        std::fclose(f);
    }

    std::error_code ec;
#ifdef _WIN32
    // std::filesystem::rename does not replace an existing target on Windows.
    if (std::filesystem::exists(path_from_utf8(path), ec)) {
        const std::string backup = backup_path(path);
        if (!MoveFileExW(path_from_utf8(path).c_str(), path_from_utf8(backup).c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            ec = std::error_code(static_cast<int>(GetLastError()), std::system_category());
        }
        // A failed backup rotation is not fatal; the new state still wins.
        ec.clear();
    }
    if (!MoveFileExW(path_from_utf8(tmp).c_str(), path_from_utf8(path).c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        ec = std::error_code(static_cast<int>(GetLastError()), std::system_category());
    }
#else
    if (std::filesystem::exists(path_from_utf8(path), ec)) {
        std::filesystem::rename(path_from_utf8(path), path_from_utf8(backup_path(path)), ec);
        // A failed backup rotation is not fatal; the new state still wins.
        ec.clear();
    }
    std::filesystem::rename(path_from_utf8(tmp), path_from_utf8(path), ec);
#endif
    if (ec) {
        std::filesystem::remove(path_from_utf8(tmp));
        if (error) *error = "cannot replace '" + path + "': " + ec.message();
        return false;
    }
    if (error) error->clear();
    return true;
}

bool load_transfer_state(const std::string& path, TransferState& out,
                         std::string* error) {
    std::string primary_error;
    if (load_from(path, out, &primary_error)) {
        if (error) error->clear();
        return true;
    }
    std::string backup_error;
    TransferState from_backup;
    if (load_from(backup_path(path), from_backup, &backup_error)) {
        out = std::move(from_backup);
        return true;
    }
    if (error) *error = primary_error + "; backup: " + backup_error;
    return false;
}

void remove_transfer_state(const std::string& path) {
    std::error_code ec;
    std::filesystem::remove(path_from_utf8(path), ec);
    std::filesystem::remove(path_from_utf8(backup_path(path)), ec);
    std::filesystem::remove(path_from_utf8(path + ".tmp"), ec);
}

}  // namespace swiftdm
