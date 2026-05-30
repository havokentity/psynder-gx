// SPDX-License-Identifier: MIT OR Apache-2.0
// Psynder-GX — lane 18 / net sub-byte bit-stream packer tests.

#include <cstring>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/Types.h"
#include "net/BitPacker.h"

using namespace psynder;
using namespace psynder::net;

TEST_CASE("net-bits: mixed-width fields round-trip exactly", "[net]") {
    BitWriter w;
    // A spread of widths including the boundaries: a single bit, an odd 13-bit
    // field, a full 32-bit value and a full 64-bit value.
    w.write_bits(5u, 3);                          // 0b101
    w.write_bits(0x1ABCu, 13);                    // 13-bit field
    w.write_bit(true);                            // single bit
    w.write_bits(0xDEADBEEFu, 32);                // full 32-bit
    w.write_uint(0xFEEDFACECAFEBEEFull, 64);      // full 64-bit
    w.flush();

    BitReader r(w.bytes());
    CHECK(r.read_bits(3) == 5u);
    CHECK(r.read_bits(13) == 0x1ABCu);
    CHECK(r.read_bit() == true);
    CHECK(r.read_bits(32) == 0xDEADBEEFu);
    CHECK(r.read_uint(64) == 0xFEEDFACECAFEBEEFull);
    CHECK(r.ok());  // never overran
}

TEST_CASE("net-bits: a single bit round-trips both values", "[net]") {
    BitWriter w;
    w.write_bit(true);
    w.write_bit(false);
    w.write_bit(true);
    w.flush();

    BitReader r(w.bytes());
    CHECK(r.read_bit() == true);
    CHECK(r.read_bit() == false);
    CHECK(r.read_bit() == true);
    CHECK(r.ok());
}

TEST_CASE("net-bits: write_bits ignores bits above bit_count", "[net]") {
    BitWriter w;
    // 0xFF has all low 8 bits set; asking for only 3 bits must store 0b111 == 7.
    w.write_bits(0xFFu, 3);
    // 0xFFFFFFFFFFFFFFFF truncated to 10 bits is 0x3FF == 1023.
    w.write_bits(~u64{0}, 10);
    w.flush();

    BitReader r(w.bytes());
    CHECK(r.read_bits(3) == 7u);
    CHECK(r.read_bits(10) == 1023u);
}

TEST_CASE("net-bits: a zero-width field writes and reads nothing", "[net]") {
    BitWriter w;
    w.write_bits(0xABCDu, 0);  // no-op
    CHECK(w.bit_count() == 0u);
    CHECK(w.byte_size() == 0u);

    BitReader r(w.bytes());
    CHECK(r.read_bits(0) == 0u);
    CHECK(r.bits_read() == 0u);
    CHECK(r.ok());
}

TEST_CASE("net-bits: byte_size is ceil(bits / 8) and flush is idempotent",
          "[net]") {
    BitWriter w;
    w.write_bits(0u, 5);  // 5 bits -> 1 byte
    CHECK(w.bit_count() == 5u);
    CHECK(w.byte_size() == 1u);

    w.write_bits(0u, 4);  // 9 bits total -> 2 bytes
    CHECK(w.bit_count() == 9u);
    CHECK(w.byte_size() == 2u);

    // flush() byte-aligns; here already aligned at the byte granularity of the
    // backing store. Calling it repeatedly must not change anything.
    const usize before = w.byte_size();
    w.flush();
    w.flush();
    CHECK(w.byte_size() == before);
    CHECK(w.bit_count() == 9u);  // flush padding never advances the bit count
}

TEST_CASE("net-bits: packing eight 5-bit values takes 5 bytes not 8", "[net]") {
    BitWriter w;
    // Eight values each needing 5 bits = 40 bits = exactly 5 bytes, versus the
    // 8 bytes a one-byte-per-value layout would cost. This is the packing WIN.
    for (u32 i = 0; i < 8; ++i) {
        w.write_bits(static_cast<u64>(i) & 0x1Fu, 5);
    }
    w.flush();
    CHECK(w.bit_count() == 40u);
    CHECK(w.byte_size() == 5u);

    BitReader r(w.bytes());
    for (u32 i = 0; i < 8; ++i) {
        CHECK(r.read_bits(5) == (static_cast<u64>(i) & 0x1Fu));
    }
    CHECK(r.ok());
}

TEST_CASE("net-bits: reading past the end returns 0 and trips ok()", "[net]") {
    BitWriter w;
    w.write_bits(0x2Au, 6);  // 6 bits in a 1-byte buffer
    w.flush();

    BitReader r(w.bytes());
    CHECK(r.read_bits(6) == 0x2Au);
    CHECK(r.ok());  // still good after the in-range read

    // The byte holds 8 bits; the upper 2 were zero-padded. Reading 2 more bits
    // stays in range (returns the pad zeros) and remains ok.
    CHECK(r.read_bits(2) == 0u);
    CHECK(r.ok());

    // Now read past the end of the single byte: missing high bits are 0 and the
    // overrun flag latches false.
    CHECK(r.read_bits(8) == 0u);
    CHECK_FALSE(r.ok());

    // ok() stays false for the life of the reader.
    CHECK(r.read_bit() == false);
    CHECK_FALSE(r.ok());
}

TEST_CASE("net-bits: a partial overrun keeps the in-range low bits", "[net]") {
    BitWriter w;
    w.write_bits(0b1011u, 4);  // exactly 4 bits; upper 4 of the byte are 0
    w.flush();

    BitReader r(w.bytes());
    // Ask for 12 bits: the buffer only has 8, so we get the 4 real bits plus 4
    // zero pad bits in range, then 4 missing (zero) bits out of range. Result is
    // just the low nibble; the overrun flag trips.
    CHECK(r.read_bits(12) == 0b1011u);
    CHECK_FALSE(r.ok());
    CHECK(r.bits_read() == 12u);  // cursor advanced across the overrun
}

TEST_CASE("net-bits: bits_needed returns the minimal width", "[net]") {
    CHECK(bits_needed(0) == 0u);    // no information -> no bits
    CHECK(bits_needed(1) == 1u);    // {0,1}
    CHECK(bits_needed(2) == 2u);    // {0..2}
    CHECK(bits_needed(3) == 2u);    // 2^2 - 1
    CHECK(bits_needed(4) == 3u);    // 2^2
    CHECK(bits_needed(7) == 3u);    // 2^3 - 1
    CHECK(bits_needed(8) == 4u);    // 2^3
    CHECK(bits_needed(255) == 8u);  // 2^8 - 1
    CHECK(bits_needed(256) == 9u);  // 2^8
    CHECK(bits_needed(~u64{0}) == 64u);  // full width

    // A value's own width is enough to hold it: every value in [0, max] fits.
    for (u64 v : {u64{5}, u64{63}, u64{1000}, u64{65535}, u64{1u << 20}}) {
        const u32 n = bits_needed(v);
        BitWriter w;
        w.write_bits(v, n);
        w.flush();
        BitReader r(w.bytes());
        CHECK(r.read_bits(n) == v);
    }
}

TEST_CASE("net-bits: zigzag maps small magnitudes to small codes", "[net]") {
    // The defining table: 0 -> 0, -1 -> 1, 1 -> 2, -2 -> 3, 2 -> 4.
    CHECK(zigzag_encode(0) == 0u);
    CHECK(zigzag_encode(-1) == 1u);
    CHECK(zigzag_encode(1) == 2u);
    CHECK(zigzag_encode(-2) == 3u);
    CHECK(zigzag_encode(2) == 4u);
}

TEST_CASE("net-bits: zigzag round-trips signed values including extremes",
          "[net]") {
    const i64 cases[] = {
        0, 1, -1, 2, -2, 127, -128, 1000, -1000,
        2147483647ll, -2147483648ll,
        9223372036854775807ll,            // INT64_MAX
        (-9223372036854775807ll - 1),     // INT64_MIN
    };
    for (i64 v : cases) {
        CHECK(zigzag_decode(zigzag_encode(v)) == v);
    }

    // And it survives a trip through the bit stream: a small negative delta packs
    // into few bits and comes back exactly.
    const i64 delta = -3;                 // zigzag -> 5, needs only 3 bits
    const u64 code  = zigzag_encode(delta);
    CHECK(code == 5u);
    const u32 n = bits_needed(code);
    CHECK(n == 3u);

    BitWriter w;
    w.write_bits(code, n);
    w.flush();
    BitReader r(w.bytes());
    CHECK(zigzag_decode(r.read_bits(n)) == delta);
}

TEST_CASE("net-bits: the same write sequence yields byte-identical buffers",
          "[net][determinism]") {
    auto build = []() {
        BitWriter w;
        w.write_bits(0xABCu, 12);
        w.write_bit(true);
        w.write_bits(zigzag_encode(-17), 6);
        w.write_uint(0x0123456789ABCDEFull, 64);
        w.write_bit(false);
        w.flush();
        return w.bytes();
    };

    const std::vector<u8> a = build();
    const std::vector<u8> b = build();

    REQUIRE(a.size() == b.size());
    CHECK(std::memcmp(a.data(), b.data(), a.size()) == 0);
}
