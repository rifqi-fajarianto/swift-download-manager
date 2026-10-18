#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace swiftdm {

// One contiguous byte range of a download. `end` is inclusive. `done` counts
// bytes already received within [start, end].
struct Segment {
    uint64_t start = 0;
    uint64_t end = 0;
    uint64_t done = 0;

    uint64_t length() const { return end >= start ? (end - start + 1) : 0; }
    bool finished() const { return done >= length(); }
    // Next byte to request from the server.
    uint64_t next_offset() const { return start + done; }
};

// Persisted per-download transfer state (spec 02-data-model.md, segments.json).
struct TransferState {
    int version = 1;
    std::string url;           // effective URL (after redirects)
    uint64_t file_size = 0;
    std::string etag;          // validator for resume
    std::string last_modified; // validator for resume
    std::vector<Segment> segments;

    uint64_t done_bytes() const;
    bool finished() const;
};

// Splits [0, file_size) into at most `connections` roughly equal contiguous
// segments. Segments never get smaller than `min_chunk` (except that a file
// smaller than min_chunk uses a single segment). A zero-size file yields an
// empty vector. Always returns at least one segment for file_size > 0.
std::vector<Segment> plan_segments(uint64_t file_size, int connections,
                                   uint64_t min_chunk = 1u << 20);

// JSON state persistence. save_* writes atomically (tmp + rename) and rotates
// the previous good file to `<path>.bak`. load_* falls back to the .bak copy
// when the primary file is missing or corrupt.
bool save_transfer_state(const std::string& path, const TransferState& state,
                         std::string* error = nullptr);
bool load_transfer_state(const std::string& path, TransferState& out,
                         std::string* error = nullptr);
void remove_transfer_state(const std::string& path);
std::string backup_path(const std::string& path);  // path + ".bak"

}  // namespace swiftdm
