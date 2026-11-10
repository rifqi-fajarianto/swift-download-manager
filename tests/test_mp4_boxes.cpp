#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "engine/mp4_boxes.h"

using namespace swiftdm;

// Helper: build a big-endian uint32 into a buffer.
static void put_u32(std::vector<uint8_t>& buf, uint32_t v) {
    buf.push_back(uint8_t(v >> 24));
    buf.push_back(uint8_t(v >> 16));
    buf.push_back(uint8_t(v >> 8));
    buf.push_back(uint8_t(v));
}

static void put_u64(std::vector<uint8_t>& buf, uint64_t v) {
    put_u32(buf, uint32_t(v >> 32));
    put_u32(buf, uint32_t(v));
}

static void test_box_type() {
    // Minimal ftyp box: size(4) + type(4) = 8 bytes.
    uint8_t data[] = {0, 0, 0, 12, 'f', 't', 'y', 'p', 0, 0, 0, 0};
    assert(box_type_at(data, sizeof(data), 0) == "ftyp");
    assert(box_type_at(data, 4, 0).empty());  // too small
    std::printf("  box_type OK\n");
}

static void test_sidx_v0() {
    std::vector<uint8_t> buf;
    // We'll build a sidx box: header + fullbox fields + data.
    std::vector<uint8_t> payload;
    // version=0, flags=0
    payload.push_back(0);
    payload.push_back(0); payload.push_back(0); payload.push_back(0);
    // reference_ID
    put_u32(payload, 1);
    // timescale
    put_u32(payload, 90000);
    // earliest_presentation_time (v0: 4 bytes)
    put_u32(payload, 0);
    // first_offset (v0: 4 bytes)
    put_u32(payload, 1000);
    // reserved
    payload.push_back(0); payload.push_back(0);
    // reference_count = 2
    payload.push_back(0); payload.push_back(2);
    // ref 1: type=0, size=50000, duration=180000, SAP
    put_u32(payload, 50000);      // reference_type=0, referenced_size=50000
    put_u32(payload, 180000);     // subsegment_duration
    put_u32(payload, 0x90000000); // SAP
    // ref 2
    put_u32(payload, 60000);
    put_u32(payload, 180000);
    put_u32(payload, 0x90000000);

    // Box header: size + 'sidx'
    uint32_t box_size = 8 + static_cast<uint32_t>(payload.size());
    put_u32(buf, box_size);
    buf.push_back('s'); buf.push_back('i'); buf.push_back('d'); buf.push_back('x');
    buf.insert(buf.end(), payload.begin(), payload.end());

    SidxBox sidx;
    assert(parse_sidx(buf.data(), buf.size(), &sidx));
    assert(sidx.timescale == 90000);
    assert(sidx.first_offset == 1000);
    assert(sidx.references.size() == 2);
    assert(sidx.references[0].referenced_size == 50000);
    assert(sidx.references[0].subsegment_duration == 180000);
    assert(!sidx.references[0].reference_type);
    assert(sidx.references[1].referenced_size == 60000);
    std::printf("  sidx_v0 OK\n");
}

static void test_moof_sequence() {
    // Build moof > mfhd with sequence_number=42.
    std::vector<uint8_t> mfhd;
    put_u32(mfhd, 16);  // box size
    mfhd.push_back('m'); mfhd.push_back('f'); mfhd.push_back('h'); mfhd.push_back('d');
    // version=0, flags=0
    put_u32(mfhd, 0);
    // sequence_number
    put_u32(mfhd, 42);

    std::vector<uint8_t> moof;
    uint32_t moof_size = 8 + static_cast<uint32_t>(mfhd.size());
    put_u32(moof, moof_size);
    moof.push_back('m'); moof.push_back('o'); moof.push_back('o'); moof.push_back('f');
    moof.insert(moof.end(), mfhd.begin(), mfhd.end());

    assert(moof_sequence_number(moof.data(), moof.size()) == 42);
    std::printf("  moof_sequence OK\n");
}

static void test_tfdt_v1() {
    // Build moof > traf > tfdt(v1) with baseMediaDecodeTime = 0x100000000.
    std::vector<uint8_t> tfdt;
    put_u32(tfdt, 20);  // box size
    tfdt.push_back('t'); tfdt.push_back('f'); tfdt.push_back('d'); tfdt.push_back('t');
    // version=1, flags=0
    tfdt.push_back(1);
    tfdt.push_back(0); tfdt.push_back(0); tfdt.push_back(0);
    put_u64(tfdt, 0x100000000ULL);

    std::vector<uint8_t> traf;
    uint32_t traf_size = 8 + static_cast<uint32_t>(tfdt.size());
    put_u32(traf, traf_size);
    traf.push_back('t'); traf.push_back('r'); traf.push_back('a'); traf.push_back('f');
    traf.insert(traf.end(), tfdt.begin(), tfdt.end());

    // mfhd placeholder
    std::vector<uint8_t> mfhd;
    put_u32(mfhd, 16);
    mfhd.push_back('m'); mfhd.push_back('f'); mfhd.push_back('h'); mfhd.push_back('d');
    put_u32(mfhd, 0);
    put_u32(mfhd, 1);

    std::vector<uint8_t> moof;
    uint32_t moof_size = 8 + static_cast<uint32_t>(mfhd.size()) +
                         static_cast<uint32_t>(traf.size());
    put_u32(moof, moof_size);
    moof.push_back('m'); moof.push_back('o'); moof.push_back('o'); moof.push_back('f');
    moof.insert(moof.end(), mfhd.begin(), mfhd.end());
    moof.insert(moof.end(), traf.begin(), traf.end());

    assert(moof_base_decode_time(moof.data(), moof.size()) == 0x100000000ULL);
    std::printf("  tfdt_v1 OK\n");
}

static void test_not_found() {
    uint8_t data[] = {0, 0, 0, 8, 'f', 't', 'y', 'p'};
    SidxBox sidx;
    assert(!parse_sidx(data, sizeof(data), &sidx));
    assert(moof_sequence_number(data, sizeof(data)) == 0);
    assert(moof_base_decode_time(data, sizeof(data)) == 0);
    std::printf("  not_found OK\n");
}

int main() {
    std::printf("mp4_boxes tests:\n");
    test_box_type();
    test_sidx_v0();
    test_moof_sequence();
    test_tfdt_v1();
    test_not_found();
    std::printf("all mp4_boxes tests passed\n");
    return 0;
}
