#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace swiftdm {

// Minimal MP4/ISOBMFF box reader for DASH SegmentBase (`sidx`) and MSE segment
// continuity checks (`mfhd` sequence number, `tfdt` base decode time).
//
// Only reads top-level and a few nested boxes; does not parse the full tree.
// All offsets/sizes are byte positions in the input buffer.

struct SidxReference {
    bool reference_type = false;  // 0 = media, 1 = index
    uint64_t referenced_size = 0;
    uint32_t subsegment_duration = 0;
};

struct SidxBox {
    uint32_t timescale = 0;
    uint64_t earliest_presentation_time = 0;
    uint64_t first_offset = 0;  // byte offset after the sidx box itself
    std::vector<SidxReference> references;
};

// Parses the first `sidx` box found in `data`. Returns false when not found or
// truncated.
bool parse_sidx(const uint8_t* data, size_t size, SidxBox* out);

// Returns the `mfhd` sequence_number from a `moof` box, or 0 if not found.
uint32_t moof_sequence_number(const uint8_t* data, size_t size);

// Returns the `tfdt` baseMediaDecodeTime from the first `traf` inside the
// first `moof`, or 0 if not found.
uint64_t moof_base_decode_time(const uint8_t* data, size_t size);

// Reads the 4-byte box type at offset (big-endian tag). Returns empty string
// if out of bounds.
std::string box_type_at(const uint8_t* data, size_t size, size_t offset);

}  // namespace swiftdm
