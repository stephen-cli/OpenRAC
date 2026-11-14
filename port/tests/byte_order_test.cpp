// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 the OpenRAC contributors
//
// The byte-order helpers (common/byte_order.h): a number's swap, a record walked field by field
// through arrays and nested records, the field count, the swap being its own inverse, and numbers
// read out of the console's bytes. The
// readers' largest and most nested records are walked too, so a record the walk cannot take fails
// here on every host, not only on a big-endian build.

#include <array>
#include <bit>
#include <cstdint>

#include "assets/disc/level.h"
#include "assets/disc/toc.h"
#include "assets/disc/volumes.h"
#include "assets/geometry/moby.h"
#include "assets/geometry/tfrag.h"
#include "assets/geometry/tie.h"
#include "common/byte_order.h"
#include "tests/check.h"

using namespace openrac::byte_order;

namespace {

// A record of the readers' shape: numbers, a byte array, a nested record and an array of records.
struct Inner {
    std::uint16_t a = 0;
    std::uint8_t b = 0;
    std::uint8_t c = 0;
};

struct Record {
    std::uint32_t word = 0;
    std::int16_t half = 0;
    std::array<std::uint8_t, 2> bytes{};
    Inner inner;
    std::array<float, 2> floats{};
    std::array<Inner, 2> inners{};
};

static_assert(sizeof(Record) == 28);

void numbers() {
    CHECK(swapped<std::uint16_t>(0x1234) == 0x3412);
    CHECK(swapped<std::uint32_t>(0x11223344) == 0x44332211);
    CHECK(swapped<std::uint64_t>(0x1122334455667788) == 0x8877665544332211);
    CHECK(swapped<std::int16_t>(-2) == -257);  // 0xfffe reversed is 0xfeff
    CHECK(byte_swapped<std::uint8_t>(0xab) == 0xab);
    CHECK(byte_swapped(1.0f) == std::bit_cast<float>(std::uint32_t{0x0000803f}));
}

void fields() {
    static_assert(field_count<Inner>() == 3);
    static_assert(field_count<Record>() == 6);
    Record r;
    r.word = 0x11223344;
    r.half = 0x0102;
    r.bytes = {0xaa, 0xbb};
    r.inner = {0x0304, 0x05, 0x06};
    r.floats = {1.0f, 2.0f};
    r.inners[1] = {0x0708, 0x09, 0x0a};
    const Record s = byte_swapped(r);
    CHECK(s.word == 0x44332211);
    CHECK(s.half == 0x0201);
    CHECK(s.bytes[0] == 0xaa && s.bytes[1] == 0xbb);
    CHECK(s.inner.a == 0x0403 && s.inner.b == 0x05 && s.inner.c == 0x06);
    CHECK(s.floats[0] == std::bit_cast<float>(std::uint32_t{0x0000803f}));
    CHECK(s.floats[1] == std::bit_cast<float>(std::uint32_t{0x00000040}));
    CHECK(s.inners[0].a == 0 && s.inners[1].a == 0x0807 && s.inners[1].c == 0x0a);

    // The swap is its own inverse, and on this host from_little_endian is the identity or the swap.
    const Record back = byte_swapped(s);
    CHECK(back.word == r.word && back.half == r.half && back.inners[1].a == r.inners[1].a);
    const Record from = from_little_endian(r);
    CHECK(from.word == (kHostIsLittleEndian ? r.word : s.word));
}

// The readers' records with the most fields and the deepest nesting walk, one field of each
// checked; that they compile is the check that the walk takes every record shape the readers have.
void readers_records() {
    using namespace openrac::assets;
    disc::LevelCoreHeader core{};
    core.tfrags = 0x11223344;
    CHECK(byte_swapped(core).tfrags == 0x44332211);
    disc::LevelHeader level{};
    level.id = 0x0a0b0c0d;
    CHECK(byte_swapped(level).id == 0x0d0c0b0a);
    disc::Shape shape{};
    shape.unused_7c = 1.0f;
    CHECK(byte_swapped(shape).unused_7c == std::bit_cast<float>(std::uint32_t{0x0000803f}));
    rac1::MobyClassHeader moby;
    moby.packet_table_offset = 0x00000102;
    CHECK(byte_swapped(moby).packet_table_offset == 0x02010000);
    rac1::TfragHeader tfrag;
    tfrag.data = 0x00000102;
    CHECK(byte_swapped(tfrag).data == 0x02010000);
    rac1::TieClassHeader tie;
    tie.normals = 0x00000102;
    CHECK(byte_swapped(tie).normals == 0x02010000);
}

// The console's bytes read where they lie: the same numbers on any host.
void console_bytes() {
    const std::uint8_t bytes[12] =
        {0x44, 0x33, 0x22, 0x11, 0x00, 0x00, 0x80, 0x3f, 0x02, 0x01, 0xfe, 0xff};
    CHECK(read_little_endian<std::uint32_t>(bytes) == 0x11223344);
    CHECK(read_little_endian<float>(bytes + 4) == 1.0f);
    CHECK(read_little_endian<std::uint16_t>(bytes + 8) == 0x0102);
    CHECK(read_little_endian<std::int16_t>(bytes + 10) == -2);
    std::uint32_t words[2] = {};
    copy_little_endian(words, bytes, 2);
    CHECK(words[0] == 0x11223344 && words[1] == 0x3f800000);
    std::uint8_t copied[3] = {};
    copy_little_endian(copied, bytes, 3);
    CHECK(copied[0] == 0x44 && copied[2] == 0x22);
}

}  // namespace

int main() {
    numbers();
    fields();
    readers_records();
    console_bytes();
    return openrac::test::result();
}
