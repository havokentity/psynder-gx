// SPDX-License-Identifier: MIT OR Apache-2.0
// Psynder-GX — lane 18 / net rUDP packet-header codec tests.
//
// Covers the classic Gaffer header: sequence + ack + 32-bit ack bitfield, its
// explicit little-endian wire layout, the ack-bitfield builder/reader, and the
// `header_acks` query — including 16-bit sequence wraparound.

#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/Types.h"
#include "net/PacketHeader.h"

using namespace psynder;
using namespace psynder::net;

TEST_CASE("net-header: encode then decode round-trips all three fields exactly",
          "[net]") {
    PacketHeader h;
    h.sequence = 0x1234;
    h.ack      = 0xABCD;
    h.ack_bits = 0xDEADBEEF;

    std::vector<u8> buf;
    encode_header(h, buf);
    REQUIRE(buf.size() == kPacketHeaderBytes);
    REQUIRE(buf.size() == 8u);

    PacketHeader got;
    REQUIRE(decode_header(buf, got));
    CHECK(got.sequence == h.sequence);
    CHECK(got.ack == h.ack);
    CHECK(got.ack_bits == h.ack_bits);
}

TEST_CASE("net-header: round-trips boundary values across the field ranges",
          "[net]") {
    const PacketHeader cases[] = {
        PacketHeader{0x0000, 0x0000, 0x00000000u},
        PacketHeader{0xFFFF, 0xFFFF, 0xFFFFFFFFu},
        PacketHeader{0x0001, 0xFFFE, 0x80000001u},
        PacketHeader{0x00FF, 0xFF00, 0x0000FF00u},
    };
    for (const PacketHeader& h : cases) {
        std::vector<u8> buf;
        encode_header(h, buf);
        PacketHeader got;
        REQUIRE(decode_header(buf, got));
        CHECK(got.sequence == h.sequence);
        CHECK(got.ack == h.ack);
        CHECK(got.ack_bits == h.ack_bits);
    }
}

TEST_CASE("net-header: decode of a too-short buffer returns false",
          "[net]") {
    PacketHeader h;
    h.sequence = 7;
    h.ack      = 9;
    h.ack_bits = 0x01020304u;

    std::vector<u8> buf;
    encode_header(h, buf);
    REQUIRE(buf.size() == 8u);

    // Every length 0..7 is too short for the 8-byte header.
    for (usize n = 0; n < kPacketHeaderBytes; ++n) {
        std::vector<u8> truncated(buf.begin(), buf.begin() + n);
        PacketHeader out;
        out.sequence = 0xDEAD;  // sentinel to confirm `out` is left untouched
        CHECK_FALSE(decode_header(truncated, out));
        CHECK(out.sequence == 0xDEAD);
    }

    // Exactly 8 bytes decodes.
    PacketHeader out;
    CHECK(decode_header(buf, out));
}

TEST_CASE("net-header: the little-endian byte layout is explicit",
          "[net]") {
    PacketHeader h;
    h.sequence = 0x0201;        // bytes 01 02
    h.ack      = 0x0403;        // bytes 03 04
    h.ack_bits = 0x08070605u;   // bytes 05 06 07 08

    std::vector<u8> buf;
    encode_header(h, buf);
    REQUIRE(buf.size() == 8u);

    CHECK(buf[0] == 0x01u);
    CHECK(buf[1] == 0x02u);
    CHECK(buf[2] == 0x03u);
    CHECK(buf[3] == 0x04u);
    CHECK(buf[4] == 0x05u);
    CHECK(buf[5] == 0x06u);
    CHECK(buf[6] == 0x07u);
    CHECK(buf[7] == 0x08u);
}

TEST_CASE("net-header: encode clears any prior contents of the output buffer",
          "[net]") {
    std::vector<u8> buf(100u, 0xAAu);  // pre-filled garbage
    PacketHeader h;
    h.sequence = 1;
    h.ack      = 2;
    h.ack_bits = 3;

    encode_header(h, buf);
    CHECK(buf.size() == kPacketHeaderBytes);  // exactly 8, garbage gone

    PacketHeader got;
    REQUIRE(decode_header(buf, got));
    CHECK(got.sequence == 1u);
    CHECK(got.ack == 2u);
    CHECK(got.ack_bits == 3u);
}

TEST_CASE("net-header: ack_bit_set reads bits and guards the range",
          "[net]") {
    const u32 bits = 0x80000001u;  // bit 0 and bit 31 set
    CHECK(ack_bit_set(bits, 0));
    CHECK_FALSE(ack_bit_set(bits, 1));
    CHECK_FALSE(ack_bit_set(bits, 30));
    CHECK(ack_bit_set(bits, 31));

    // Out-of-range indices are false, never UB.
    CHECK_FALSE(ack_bit_set(bits, 32));
    CHECK_FALSE(ack_bit_set(bits, 1000));

    // A walking-bit sweep: bit i set iff queried index == i.
    for (u32 i = 0; i < 32; ++i) {
        const u32 single = (u32{1} << i);
        CHECK(ack_bit_set(single, i));
        CHECK_FALSE(ack_bit_set(single, (i + 1) % 32));
    }
}

TEST_CASE("net-header: build_ack_bits sets the right bits for a received set",
          "[net]") {
    // ack == 100. Bit i represents sequence (ack - 1 - i):
    //   bit 0 -> 99, bit 1 -> 98, bit 2 -> 97, ... bit 31 -> 68.
    const u16 ack = 100;
    const u16 received[] = {
        100,  // == ack itself: NOT a bit (it is the header's ack field)
        99,   // bit 0
        98,   // bit 1
        97,   // bit 2
        68,   // bit 31 (the oldest addressable)
        67,   // 33 behind ack -> out of the 32-slot window, no bit
        101,  // ahead of ack -> no bit
    };
    const u32 bits = build_ack_bits(ack, received);

    CHECK(ack_bit_set(bits, 0));
    CHECK(ack_bit_set(bits, 1));
    CHECK(ack_bit_set(bits, 2));
    CHECK(ack_bit_set(bits, 31));
    CHECK_FALSE(ack_bit_set(bits, 3));   // 96 was not received
    CHECK_FALSE(ack_bit_set(bits, 30));  // 69 was not received

    // ack itself, the too-old, and the ahead sequence contributed nothing
    // beyond the four expected bits.
    const u32 expected =
        (u32{1} << 0) | (u32{1} << 1) | (u32{1} << 2) | (u32{1} << 31);
    CHECK(bits == expected);
}

TEST_CASE("net-header: build_ack_bits handles the 16-bit sequence wrap",
          "[net]") {
    // ack == 1. The 32 prior sequences wrap below zero into the top of the
    // 16-bit space. Bit i represents (ack - 1 - i):
    //   bit 0  -> 1 - 1      == 0
    //   bit 1  -> 1 - 2      == 65535 (0xFFFF)
    //   bit 2  -> 1 - 3      == 65534 (0xFFFE)
    //   bit 31 -> 1 - 32     == 65505 (0xFFE1)
    const u16 ack = 1;
    const u16 received[] = {0, u16(0xFFFF), u16(0xFFFE), u16(0xFFE1)};

    const u32 bits = build_ack_bits(ack, received);
    CHECK(ack_bit_set(bits, 0));
    CHECK(ack_bit_set(bits, 1));
    CHECK(ack_bit_set(bits, 2));
    CHECK(ack_bit_set(bits, 31));

    const u32 expected =
        (u32{1} << 0) | (u32{1} << 1) | (u32{1} << 2) | (u32{1} << 31);
    CHECK(bits == expected);
}

TEST_CASE("net-header: header_acks is true for the ack and the flagged bits",
          "[net]") {
    PacketHeader h;
    h.ack = 200;
    // Mark bits 0, 5, 31 -> sequences 199, 194, 168.
    h.ack_bits = (u32{1} << 0) | (u32{1} << 5) | (u32{1} << 31);

    // The ack itself is always acknowledged.
    CHECK(header_acks(h, 200));

    // Sequences flagged in ack_bits.
    CHECK(header_acks(h, 199));  // bit 0  (ack - 1)
    CHECK(header_acks(h, 194));  // bit 5  (ack - 6)
    CHECK(header_acks(h, 168));  // bit 31 (ack - 32)

    // Un-acked: in-window but their bit is clear.
    CHECK_FALSE(header_acks(h, 198));  // bit 1, not set
    CHECK_FALSE(header_acks(h, 195));  // bit 4, not set
    CHECK_FALSE(header_acks(h, 169));  // bit 30, not set

    // Out of the 32-slot window entirely.
    CHECK_FALSE(header_acks(h, 167));  // ack - 33, no addressable bit
    CHECK_FALSE(header_acks(h, 201));  // ahead of ack
    CHECK_FALSE(header_acks(h, 50));   // far behind
}

TEST_CASE("net-header: header_acks respects the 16-bit wrap",
          "[net]") {
    PacketHeader h;
    h.ack = 2;
    // bit 0 -> seq 1, bit 2 -> seq 65535 (2 - 3 wraps), bit 4 -> seq 65533.
    h.ack_bits = (u32{1} << 0) | (u32{1} << 2) | (u32{1} << 4);

    CHECK(header_acks(h, 2));            // the ack itself
    CHECK(header_acks(h, 1));            // bit 0
    CHECK(header_acks(h, u16(0xFFFF)));  // bit 2, wrapped (2 - 3)
    CHECK(header_acks(h, u16(0xFFFD)));  // bit 4, wrapped (2 - 5)

    CHECK_FALSE(header_acks(h, 0));            // bit 1, not set
    CHECK_FALSE(header_acks(h, u16(0xFFFE)));  // bit 3, not set
    CHECK_FALSE(header_acks(h, 3));            // ahead of the ack
}

TEST_CASE("net-header: build_ack_bits and header_acks agree end to end",
          "[net]") {
    const u16 ack = 5000;
    const u16 received[] = {5000, 4999, 4998, 4990, 4969 /* ack-31 */, 4968 /* ack-32 */};
    const u32 bits = build_ack_bits(ack, received);

    PacketHeader h;
    h.sequence = 12345;
    h.ack      = ack;
    h.ack_bits = bits;

    // Everything in the received set within the addressable window is acked.
    CHECK(header_acks(h, 5000));  // the ack
    CHECK(header_acks(h, 4999));  // bit 0
    CHECK(header_acks(h, 4998));  // bit 1
    CHECK(header_acks(h, 4990));  // bit 9
    CHECK(header_acks(h, 4969));  // bit 30
    CHECK(header_acks(h, 4968));  // bit 31

    // A neighbour that was never received is not acked.
    CHECK_FALSE(header_acks(h, 4997));  // bit 2, never received
}

TEST_CASE("net-header: same header encodes to identical bytes",
          "[net][determinism]") {
    PacketHeader h;
    h.sequence = 0xCAFE;
    h.ack      = 0xBEEF;
    h.ack_bits = 0x12345678u;

    std::vector<u8> a;
    std::vector<u8> b;
    encode_header(h, a);
    encode_header(h, b);

    REQUIRE(a.size() == b.size());
    REQUIRE(a.size() == kPacketHeaderBytes);
    for (usize i = 0; i < a.size(); ++i) {
        CHECK(a[i] == b[i]);
    }

    // And a decode of either reproduces the original header.
    PacketHeader ga;
    PacketHeader gb;
    REQUIRE(decode_header(a, ga));
    REQUIRE(decode_header(b, gb));
    CHECK(ga.sequence == gb.sequence);
    CHECK(ga.ack == gb.ack);
    CHECK(ga.ack_bits == gb.ack_bits);
}
