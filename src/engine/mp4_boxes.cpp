#include "engine/mp4_boxes.h"

#include <cstring>

namespace swiftdm {
namespace {

uint32_t read_u32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

uint64_t read_u64(const uint8_t* p) {
    return (uint64_t(read_u32(p)) << 32) | uint64_t(read_u32(p + 4));
}

// Iterates top-level boxes. Calls fn(type, payload, payload_size, box_offset)
// for each box. fn returns false to stop.
template <typename Fn>
void walk_boxes(const uint8_t* data, size_t size, Fn&& fn) {
    size_t offset = 0;
    while (offset + 8 <= size) {
        uint64_t box_size = read_u32(data + offset);
        const char* type = reinterpret_cast<const char*>(data + offset + 4);
        size_t header = 8;
        if (box_size == 1) {
            if (offset + 16 > size) break;
            box_size = read_u64(data + offset + 8);
            header = 16;
        } else if (box_size == 0) {
            box_size = size - offset;
        }
        if (box_size < header || offset + box_size > size) break;
        const uint8_t* payload = data + offset + header;
        const size_t payload_size = static_cast<size_t>(box_size - header);
        std::string tag(type, 4);
        if (!fn(tag, payload, payload_size, offset)) return;
        offset += static_cast<size_t>(box_size);
    }
}

}  // namespace

std::string box_type_at(const uint8_t* data, size_t size, size_t offset) {
    if (offset + 8 > size) return {};
    return std::string(reinterpret_cast<const char*>(data + offset + 4), 4);
}

bool parse_sidx(const uint8_t* data, size_t size, SidxBox* out) {
    if (!out) return false;
    *out = SidxBox{};
    bool found = false;
    walk_boxes(data, size, [&](const std::string& type, const uint8_t* payload,
                               size_t psize, size_t /*offset*/) -> bool {
        if (type != "sidx") return true;
        if (psize < 12) return true;
        uint8_t version = payload[0];
        // skip flags (3 bytes), reference_ID (4 bytes)
        size_t p = 4 + 4;
        out->timescale = read_u32(payload + p);
        p += 4;
        if (version == 0) {
            if (p + 8 > psize) return true;
            out->earliest_presentation_time = read_u32(payload + p);
            p += 4;
            out->first_offset = read_u32(payload + p);
            p += 4;
        } else {
            if (p + 16 > psize) return true;
            out->earliest_presentation_time = read_u64(payload + p);
            p += 8;
            out->first_offset = read_u64(payload + p);
            p += 8;
        }
        // reserved (2 bytes)
        p += 2;
        if (p + 2 > psize) return true;
        uint16_t count = static_cast<uint16_t>((payload[p] << 8) | payload[p + 1]);
        p += 2;
        for (uint16_t i = 0; i < count && p + 12 <= psize; ++i) {
            SidxReference ref;
            uint32_t w = read_u32(payload + p);
            ref.reference_type = (w >> 31) != 0;
            ref.referenced_size = w & 0x7FFFFFFFu;
            p += 4;
            ref.subsegment_duration = read_u32(payload + p);
            p += 4;
            // SAP fields (4 bytes) — skip
            p += 4;
            out->references.push_back(ref);
        }
        found = true;
        return false;
    });
    return found;
}

uint32_t moof_sequence_number(const uint8_t* data, size_t size) {
    uint32_t seq = 0;
    walk_boxes(data, size, [&](const std::string& type, const uint8_t* payload,
                               size_t psize, size_t /*offset*/) -> bool {
        if (type != "moof") return true;
        // Walk children inside moof looking for mfhd.
        walk_boxes(payload, psize, [&](const std::string& inner_type,
                                       const uint8_t* inner_payload,
                                       size_t inner_size,
                                       size_t /*inner_offset*/) -> bool {
            if (inner_type != "mfhd") return true;
            if (inner_size < 8) return true;
            // version(1) + flags(3) + sequence_number(4)
            seq = read_u32(inner_payload + 4);
            return false;
        });
        return false;
    });
    return seq;
}

uint64_t moof_base_decode_time(const uint8_t* data, size_t size) {
    uint64_t bdt = 0;
    walk_boxes(data, size, [&](const std::string& type, const uint8_t* payload,
                               size_t psize, size_t /*offset*/) -> bool {
        if (type != "moof") return true;
        // Walk children looking for traf.
        walk_boxes(payload, psize, [&](const std::string& inner_type,
                                       const uint8_t* inner_payload,
                                       size_t inner_size,
                                       size_t /*inner_offset*/) -> bool {
            if (inner_type != "traf") return true;
            // Walk traf children looking for tfdt.
            walk_boxes(inner_payload, inner_size,
                       [&](const std::string& traf_type,
                           const uint8_t* traf_payload, size_t traf_size,
                           size_t /*traf_offset*/) -> bool {
                           if (traf_type != "tfdt") return true;
                           if (traf_size < 4) return true;
                           uint8_t version = traf_payload[0];
                           if (version == 0) {
                               if (traf_size < 8) return true;
                               bdt = read_u32(traf_payload + 4);
                           } else {
                               if (traf_size < 12) return true;
                               bdt = read_u64(traf_payload + 4);
                           }
                           return false;
                       });
            return false;
        });
        return false;
    });
    return bdt;
}

}  // namespace swiftdm
