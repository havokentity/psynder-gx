// SPDX-License-Identifier: MIT OR Apache-2.0
// Psynder-GX — rUDP packet-header codec (see PacketHeader.h). Lane 18 (net).
//
// All integer fields are encoded / decoded byte-by-byte in explicit little-
// endian order; we never memcpy the struct, so the wire bytes do not depend on
// host endianness or struct padding (DESIGN-PSYNDER-GX.md §14 determinism).

#include "net/PacketHeader.h"

namespace psynder::net {

namespace {

// Append a u16 in little-endian (low byte first).
PSY_FORCEINLINE void put_u16_le(std::vector<u8>& out, u16 v) {
    out.push_back(static_cast<u8>(v & 0xFFu));
    out.push_back(static_cast<u8>((v >> 8) & 0xFFu));
}

// Append a u32 in little-endian (low byte first).
PSY_FORCEINLINE void put_u32_le(std::vector<u8>& out, u32 v) {
    out.push_back(static_cast<u8>(v & 0xFFu));
    out.push_back(static_cast<u8>((v >> 8) & 0xFFu));
    out.push_back(static_cast<u8>((v >> 16) & 0xFFu));
    out.push_back(static_cast<u8>((v >> 24) & 0xFFu));
}

// Read a little-endian u16 from `p` (caller guarantees >= 2 bytes available).
PSY_FORCEINLINE u16 get_u16_le(const u8* p) noexcept {
    return static_cast<u16>(static_cast<u16>(p[0]) |
                            (static_cast<u16>(p[1]) << 8));
}

// Read a little-endian u32 from `p` (caller guarantees >= 4 bytes available).
PSY_FORCEINLINE u32 get_u32_le(const u8* p) noexcept {
    return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) |
           (static_cast<u32>(p[2]) << 16) | (static_cast<u32>(p[3]) << 24);
}

}  // namespace

void encode_header(const PacketHeader& h, std::vector<u8>& out) {
    out.clear();
    out.reserve(kPacketHeaderBytes);
    put_u16_le(out, h.sequence);
    put_u16_le(out, h.ack);
    put_u32_le(out, h.ack_bits);
}

bool decode_header(std::span<const u8> bytes, PacketHeader& out) noexcept {
    if (bytes.size() < kPacketHeaderBytes) {
        return false;
    }
    const u8* p = bytes.data();
    out.sequence = get_u16_le(p + 0);
    out.ack      = get_u16_le(p + 2);
    out.ack_bits = get_u32_le(p + 4);
    return true;
}

u32 build_ack_bits(u16 ack, std::span<const u16> received_seqs) noexcept {
    u32 bits = 0;
    for (const u16 seq : received_seqs) {
        // Distance ahead of `seq`: positive => `seq` is behind `ack`.
        const i32 d = seq_diff(ack, seq);  // ack - seq, modular-shortest
        // Only the 32 sequences strictly before `ack` map to a bit; bit i is
        // `ack - 1 - i`, i.e. distance d in [1,32] -> bit index d-1.
        if (d >= 1 && d <= 32) {
            bits |= (u32{1} << static_cast<u32>(d - 1));
        }
    }
    return bits;
}

bool ack_bit_set(u32 ack_bits, u32 i) noexcept {
    if (i >= 32) {
        return false;
    }
    return ((ack_bits >> i) & 1u) != 0u;
}

bool header_acks(const PacketHeader& h, u16 seq) noexcept {
    if (seq == h.ack) {
        return true;
    }
    const i32 d = seq_diff(h.ack, seq);  // h.ack - seq, modular-shortest
    if (d >= 1 && d <= 32) {
        return ack_bit_set(h.ack_bits, static_cast<u32>(d - 1));
    }
    return false;
}

}  // namespace psynder::net
