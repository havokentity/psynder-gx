// SPDX-License-Identifier: MIT OR Apache-2.0
// Psynder-GX — portable sub-byte bit-stream packer/unpacker. Lane 18 (net).
// See BitPacker.h for the bit order (LSB-first within a byte) + determinism.

#include "net/BitPacker.h"

namespace psynder::net {

namespace {

// Mask isolating the low `n` bits of a u64, for n in [0, 64]. The naive
// `(1ull << n) - 1` is undefined for n == 64 (shift width == operand width), so
// we special-case the full-width mask. Pure integer; no UB on any platform.
inline u64 low_mask(u32 n) noexcept {
    return (n >= 64) ? ~u64{0} : ((u64{1} << n) - 1);
}

}  // namespace

// ── BitWriter ──────────────────────────────────────────────────────────────

void BitWriter::write_bits(u64 value, u32 bit_count) {
    if (bit_count == 0) return;          // a zero-width field writes nothing.
    if (bit_count > 64) bit_count = 64;  // contract clamp; never shift past 64.

    // Mask off any high bits of `value` beyond the field width so the caller may
    // pass a wide value with a narrow width (write_bits(0xFF, 3) -> 0b111).
    value &= low_mask(bit_count);

    // Emit one bit at a time, LSB of `value` first, into the LSB-first byte
    // stream. The bit position within the current byte is bit_count_ % 8; a new
    // byte is appended whenever we land on a byte boundary. Doing it bit-by-bit
    // keeps the placement explicit and obviously endianness-independent — there
    // is no wider store whose byte order could vary by platform.
    for (u32 i = 0; i < bit_count; ++i) {
        const u32 bit_in_byte = static_cast<u32>(bit_count_ & 7);
        if (bit_in_byte == 0) {
            bytes_.push_back(0);  // starting a fresh byte; zero-initialised.
        }
        const u64 bit = (value >> i) & 1;
        bytes_.back() |= static_cast<u8>(bit << bit_in_byte);
        ++bit_count_;
    }
}

void BitWriter::write_bit(bool b) {
    write_bits(b ? u64{1} : u64{0}, 1);
}

void BitWriter::flush() {
    // The backing vector already holds ceil(bit_count_ / 8) bytes; any unused
    // high bits of the final byte were left zero by write_bits (we OR fresh bits
    // into a zero-initialised byte). So byte-alignment is already satisfied and
    // there is nothing to pad — flush() is a no-op kept for API symmetry and
    // explicit caller intent. It is trivially idempotent.
}

// ── BitReader ──────────────────────────────────────────────────────────────

u64 BitReader::read_bits(u32 bit_count) {
    if (bit_count == 0) return 0;        // a zero-width field yields nothing.
    if (bit_count > 64) bit_count = 64;  // contract clamp; mirrors the writer.

    const usize total_bits = bytes_.size() * 8;

    // Reassemble `bit_count` bits, lowest first, into `result`. A bit beyond the
    // buffer end contributes 0 (left as-is in `result`) and latches the overrun
    // flag, but the cursor still advances so the caller's field layout stays
    // consistent across a truncated read.
    u64 result = 0;
    for (u32 i = 0; i < bit_count; ++i) {
        if (bit_pos_ < total_bits) {
            const usize byte_index   = bit_pos_ >> 3;
            const u32   bit_in_byte  = static_cast<u32>(bit_pos_ & 7);
            const u64   bit = (bytes_[byte_index] >> bit_in_byte) & 1;
            result |= bit << i;
        } else {
            ok_ = false;  // read past the end — missing high bits stay 0.
        }
        ++bit_pos_;
    }
    return result;
}

bool BitReader::read_bit() {
    return read_bits(1) != 0;
}

// ── Helpers ────────────────────────────────────────────────────────────────

u32 bits_needed(u64 max_value) noexcept {
    // ceil(log2(max_value + 1)) via a pure integer shift loop. A field whose
    // only value is 0 needs no bits (bits_needed(0) == 0); otherwise count the
    // shifts until max_value reaches 0. For max_value == UINT64_MAX this returns
    // 64 (the loop shifts a value with bit 63 set down to 0 in 64 steps).
    u32 bits = 0;
    while (max_value != 0) {
        max_value >>= 1;
        ++bits;
    }
    return bits;
}

u64 zigzag_encode(i64 value) noexcept {
    // Interleave sign into the low bit: non-negative n -> 2n, negative n ->
    // -2n-1, so small magnitudes map to small codes. The shifts are on the u64
    // bit pattern (defined for the full i64 range, incl. INT64_MIN), avoiding
    // UB from shifting a signed negative.
    const u64 bits = static_cast<u64>(value);
    // (value << 1) ^ (value >> 63), computed on the unsigned bit pattern. The
    // arithmetic-right-shift of a negative is replicated here as ~0 / 0 via a
    // signed shift on `value`, which is implementation-defined-but-universal for
    // two's-complement targets (the only kind Psynder-GX supports).
    return (bits << 1) ^ static_cast<u64>(value >> 63);
}

i64 zigzag_decode(u64 value) noexcept {
    // Inverse interleave: (value >> 1) ^ -(value & 1). The negate of the low bit
    // produces 0 or all-ones, flipping the magnitude back to its signed form.
    return static_cast<i64>((value >> 1) ^ (~(value & 1) + 1));
}

}  // namespace psynder::net
