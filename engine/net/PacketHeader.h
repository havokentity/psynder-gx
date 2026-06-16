// SPDX-License-Identifier: MIT OR Apache-2.0
// Psynder-GX — the classic Gaffer-on-Games rUDP packet header. Lane 18 (net).
//
// Every datagram carries an 8-byte header describing what the SENDER knows
// about the connection's two sequence streams:
//
//   sequence — this packet's own 16-bit sequence number (the local outbound
//              counter, wrapping at 65536 — compare with `seq_greater`).
//   ack      — the most recent sequence the sender has RECEIVED from the peer.
//   ack_bits — a 32-bit redundant acknowledgement of the 32 sequences *before*
//              `ack`. Bit i (0..31) set => sequence `ack - 1 - i` was also
//              received (16-bit wraparound subtraction). So one header acks up
//              to 33 distinct peer sequences: `ack` itself plus the 32 named by
//              the bitfield. Sending the recent-history bitfield in every packet
//              is what makes acks robust to loss — a single delivered packet
//              re-confirms a whole window, so an ack does not need its own
//              retransmit.
//
// This is the on-the-wire HEADER primitive. The send/recv *reliability* policy
// (in-flight ring, retransmit, de-dupe) lives in Reliability.h and builds on
// top of this header; PacketHeader does not duplicate it. The 16-bit modular
// sequence helpers (`seq_greater`, `seq_diff`) come from SequenceBuffer.h.
//
// Wire format (FIXED, explicit little-endian — independent of host byte order
// or struct padding; we never memcpy the struct):
//   offset 0..1 : u16 sequence  (LE)
//   offset 2..3 : u16 ack       (LE)
//   offset 4..7 : u32 ack_bits  (LE)
//   total       : 8 bytes (kPacketHeaderBytes)
//
// Determinism / safety: strict-FP net lane (-fno-fast-math -ffp-contract=off,
// DESIGN-PSYNDER-GX.md §14). Every operation here is pure integer shift / mask
// plus the modular seq utilities — no floats, no transcendentals, no RNG. encode
// then decode round-trips a header EXACTLY, and the same header always produces
// byte-identical output.

#pragma once

#include "core/Types.h"
#include "net/SequenceBuffer.h"

#include <span>
#include <vector>

namespace psynder::net {

// ──────────────────────────────────────────────────────────────────────────
// PacketHeader — the three fields carried by every datagram. See the banner.
//   sequence : this packet's own outbound sequence number.
//   ack      : the latest sequence received FROM the peer.
//   ack_bits : bit i set => (ack - 1 - i) was also received (16-bit wrap).
// ──────────────────────────────────────────────────────────────────────────
struct PacketHeader {
    u16 sequence = 0;
    u16 ack      = 0;
    u32 ack_bits = 0;
};

// Exact on-the-wire size of an encoded header: u16 sequence + u16 ack + u32
// ack_bits = 8 bytes.
inline constexpr usize kPacketHeaderBytes = sizeof(u16) * 2 + sizeof(u32);

// ──────────────────────────────────────────────────────────────────────────
// encode_header — serialise `h` to exactly `kPacketHeaderBytes` (8) bytes in
// explicit little-endian order (sequence, ack, ack_bits). `out` is CLEARED
// first, then the 8 header bytes are written — on return `out.size()` == 8.
// Pure integer byte shuffling: the bytes are identical on every platform.
// ──────────────────────────────────────────────────────────────────────────
void encode_header(const PacketHeader& h, std::vector<u8>& out);

// ──────────────────────────────────────────────────────────────────────────
// decode_header — inverse of `encode_header`. Reads the first 8 little-endian
// bytes of `bytes` into `out` (sequence, ack, ack_bits) and returns true. If
// `bytes` holds fewer than `kPacketHeaderBytes` bytes it returns false and
// leaves `out` untouched. Extra trailing bytes (the packet payload) are
// ignored — only the leading header is consumed.
// ──────────────────────────────────────────────────────────────────────────
bool decode_header(std::span<const u8> bytes, PacketHeader& out) noexcept;

// ──────────────────────────────────────────────────────────────────────────
// build_ack_bits — given the latest received sequence `ack` and a list of
// recently-received peer sequence numbers, produce the 32-bit ack bitfield.
// Bit i (0..31) is set iff `ack - 1 - i` (under 16-bit wraparound) appears in
// `received_seqs`. `ack` itself is NOT represented in the bitfield (it is the
// header's `ack` field); only the 32 sequences strictly before it are. A
// sequence in `received_seqs` that is `ack` itself, ahead of `ack`, or more
// than 32 behind it contributes no bit. Pure integer + seq arithmetic.
// ──────────────────────────────────────────────────────────────────────────
u32 build_ack_bits(u16 ack, std::span<const u16> received_seqs) noexcept;

// ──────────────────────────────────────────────────────────────────────────
// ack_bit_set — is bit `i` (0..31) of `ack_bits` set? `i` outside [0,31]
// returns false. Equivalent to (ack_bits >> i) & 1, with the range guard.
// ──────────────────────────────────────────────────────────────────────────
bool ack_bit_set(u32 ack_bits, u32 i) noexcept;

// ──────────────────────────────────────────────────────────────────────────
// header_acks — does header `h` acknowledge sequence `seq`? True iff
//   seq == h.ack                         (the directly-acked latest), OR
//   seq == h.ack - 1 - i for some bit i set in h.ack_bits.
// The bit offset is derived with `seq_diff(h.ack, seq) - 1`: for a `seq`
// strictly behind `h.ack` the modular distance d = seq_diff(h.ack, seq) is in
// [1,32] for the 32 addressable history slots, mapping to bit index d-1. A
// `seq` equal to `h.ack`, ahead of it, or more than 32 behind is acked only if
// it equals `h.ack`. Pure integer + seq arithmetic; safe across the 16-bit
// wrap.
// ──────────────────────────────────────────────────────────────────────────
bool header_acks(const PacketHeader& h, u16 seq) noexcept;

}  // namespace psynder::net
