// SPDX-License-Identifier: MIT OR Apache-2.0
// Psynder-GX — portable sub-byte bit-stream packer/unpacker. Lane 18 (net).
//
// SnapshotPack.h serialises a quantized snapshot byte-granularly: each field is
// a whole 32-bit little-endian integer. But once the floats have been collapsed
// to integers the real bandwidth win is *sub-byte* — a yaw in milli-degrees fits
// in 19 bits, a per-tick position delta in a handful, an entity flag in one. The
// SnapshotPack / SnapshotMetrics headers keep deferring exactly this: "the
// integers a downstream delta/bit-packer can compress far harder than raw f32".
// BitPacker is that downstream layer — the bit-level complement that turns the
// quantized integers into a compact wire payload.
//
// Bit order (FIXED BY THE CODE, not by host endianness):
//   - Bits are packed LSB-FIRST WITHIN EACH BYTE. The first bit written lands in
//     bit 0 (the 0x01 mask) of byte 0; the eighth bit written fills bit 7 (0x80)
//     of byte 0; the ninth starts bit 0 of byte 1, and so on.
//   - A multi-bit field's own low bit is written first: write_bits(value, n)
//     emits bit 0 of `value`, then bit 1, ... up to bit n-1. The reader pulls
//     them back in the same order and reassembles `value`'s low bits.
//   This is a single, explicit convention realised with pure integer shifts and
//   masks — no memcpy of a wider type, no reliance on the platform's byte order
//   or struct padding. The same write sequence therefore produces byte-identical
//   output on a big-endian and a little-endian machine, satisfying the lockstep /
//   server-authoritative determinism contract (DESIGN-PSYNDER-GX.md §14).
//
// Determinism / safety: strict-FP net lane (-fno-fast-math -ffp-contract=off).
// Every operation here is pure integer shift/mask/compare — no floats, no
// transcendentals, no RNG. The only heap is the writer's growing output vector
// (amortised push_back); a read never allocates. write_bits/read_bits round-trip
// EXACTLY for any bit_count in [0, 64].

#pragma once

#include "core/Types.h"

#include <span>
#include <vector>

namespace psynder::net {

// ──────────────────────────────────────────────────────────────────────────
// BitWriter — appends bits, LSB-first within each byte (see the header banner),
// into an internally owned, growing byte buffer. The caller reads the finished
// payload back out via `bytes()`. Construction is allocation-free; the buffer
// grows by amortised push_back as bits accumulate.
// ──────────────────────────────────────────────────────────────────────────
class BitWriter {
public:
    BitWriter() = default;

    // Write the low `bit_count` bits (0..64) of `value`, LSB of `value` first.
    // Bits of `value` at or above `bit_count` are masked off and ignored, so a
    // caller may pass a wide value and a narrow width safely (write_bits(0xFF, 3)
    // stores 0b111 == 7). bit_count == 0 is a no-op. Passing bit_count > 64 is a
    // contract violation and is clamped to 64.
    void write_bits(u64 value, u32 bit_count);

    // `write_uint` is the canonical name; `write_bits` is the historical alias.
    // Both are the same operation.
    void write_uint(u64 value, u32 bit_count) { write_bits(value, bit_count); }

    // Write a single bit (true == 1). Equivalent to write_bits(b ? 1 : 0, 1).
    void write_bit(bool b);

    // Pad the final partial byte with zero bits so `bytes()` is byte-aligned and
    // `byte_size()` whole bytes are complete. Idempotent: calling flush() when
    // already byte-aligned does nothing and never appends a spurious zero byte.
    void flush();

    // Total bits written so far (independent of flush padding — flush pads the
    // backing bytes but does NOT advance the logical bit count).
    usize bit_count() const noexcept { return bit_count_; }

    // Number of whole bytes currently backing the stream = ceil(bit_count / 8).
    usize byte_size() const noexcept { return bytes_.size(); }

    // The finished payload. After the last write you will usually call flush()
    // first so the trailing partial byte is zero-padded and stable.
    const std::vector<u8>& bytes() const noexcept { return bytes_; }

private:
    std::vector<u8> bytes_;       // LSB-first packed output, grows as needed.
    usize           bit_count_ = 0;  // total logical bits written.
};

// ──────────────────────────────────────────────────────────────────────────
// BitReader — pulls bits back out of a byte span in the SAME order BitWriter
// wrote them (LSB-first within each byte). Reading past the end of the buffer is
// not undefined: the missing high bits read back as 0 and `ok()` latches false,
// so a truncated/malformed payload degrades predictably instead of crashing.
// ──────────────────────────────────────────────────────────────────────────
class BitReader {
public:
    explicit BitReader(std::span<const u8> bytes) noexcept : bytes_(bytes) {}

    // Read `bit_count` bits (0..64) and reassemble them into a u64, lowest bit
    // first (the inverse of write_bits). bit_count == 0 returns 0. If the read
    // runs past the end of the buffer the bits that exist are returned in their
    // correct positions, every missing higher bit reads as 0, and `ok()` becomes
    // false. bit_count > 64 is clamped to 64.
    u64 read_bits(u32 bit_count);

    // `read_uint` mirrors BitWriter::write_uint; alias of read_bits.
    u64 read_uint(u32 bit_count) { return read_bits(bit_count); }

    // Read a single bit as a bool. Past the end returns false and trips `ok()`.
    bool read_bit();

    // True until a read has run past the end of the buffer (an overrun). Once
    // tripped it stays false for the life of the reader.
    bool ok() const noexcept { return ok_; }

    // Total bits consumed so far, including bits "read" past the end (those still
    // advance the cursor, so a caller can keep a consistent field layout even on
    // a truncated buffer).
    usize bits_read() const noexcept { return bit_pos_; }

private:
    std::span<const u8> bytes_;
    usize               bit_pos_ = 0;     // next bit index to read.
    bool                ok_      = true;  // false once any read overran.
};

// ──────────────────────────────────────────────────────────────────────────
// bits_needed — the minimal number of bits required to represent every value in
// [0, max_value], i.e. ceil(log2(max_value + 1)) computed with a pure integer
// shift loop (no std::log). bits_needed(0) == 0: a field whose only possible
// value is 0 carries no information and needs no bits. Boundary behaviour:
// bits_needed(2^k - 1) == k and bits_needed(2^k) == k + 1.
// ──────────────────────────────────────────────────────────────────────────
u32 bits_needed(u64 max_value) noexcept;

// ──────────────────────────────────────────────────────────────────────────
// zigzag_encode / zigzag_decode — map signed integers onto unsigned so that
// SMALL MAGNITUDES (of either sign) become SMALL CODES, which then pack into few
// bits via write_bits. This is the standard zig-zag interleave:
//   0 -> 0, -1 -> 1, 1 -> 2, -2 -> 3, 2 -> 4, ...
// so a tiny frame-to-frame position/yaw delta of ±1 step costs ~1 bit instead of
// the full sign-extended width. Pure integer arithmetic; an exact round-trip
// over the entire i64 range (decode(encode(x)) == x).
// ──────────────────────────────────────────────────────────────────────────
u64 zigzag_encode(i64 value) noexcept;
i64 zigzag_decode(u64 value) noexcept;

}  // namespace psynder::net
