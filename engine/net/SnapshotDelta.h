// SPDX-License-Identifier: MIT OR Apache-2.0
// Psynder-GX — snapshot delta codec (XOR + RLE). Lane 18 (net).
//
// DESIGN-PSYNDER-GX.md §10.4:
//   "Snapshot delta compression — XOR-style delta against the last acked
//    baseline."
//
// Wire encoding for a delta payload:
//
//   1. XOR `current` against `baseline` byte-by-byte. Equal bytes become 0.
//   2. RLE the zero runs. Non-zero bytes are emitted verbatim, but runs of
//      zeros are compressed to `0x00, run_length-1` (so a run of N zeros
//      where N >= 1 becomes 2 bytes total, and any single zero is encoded
//      as `0x00, 0x00`). Non-zero bytes can never collide with the
//      escape sequence because the escape itself starts with 0x00.
//
// Encoding tag interpretation:
//   byte == 0x00 :  followed by ONE more byte `n` (0..255) representing
//                   run length `n + 1`. So `0x00 0x00` = 1 zero,
//                   `0x00 0xFF` = 256 zeros. Long runs split into multiple
//                   chunks (the cap is one byte per chunk).
//   byte != 0x00 :  literal output byte.
//
// Properties:
//   - Identical snapshots compress to a single `0x00 0xFF` pair every 256
//     bytes — the all-zeros worst case is 1/128 of the original size.
//   - Worst case (every byte differs and non-zero): 1:1 with the input.
//   - Worst-case expansion: 0% (a non-zero byte stream copies straight
//     through). The codec never grows the data.
//
// Compile flag: -fno-fast-math -ffp-contract=off (DESIGN §14, lockstep
// determinism). No float math here, but the flag is uniform across lane 18.

#pragma once

#include "core/Types.h"

#include <span>
#include <vector>

namespace psynder::net {

// ──────────────────────────────────────────────────────────────────────────
// xor_delta_encode — XOR `current` against `baseline`, RLE-compress, append
// to `out` (existing bytes preserved). Returns the number of bytes appended.
//
// If `baseline.size() != current.size()`, the missing tail (whichever side
// is shorter) is treated as zero-extension. In practice the caller passes
// equal-length buffers — snapshots are fixed-size for a given map.
// ──────────────────────────────────────────────────────────────────────────
usize xor_delta_encode(std::span<const u8> baseline,
                       std::span<const u8> current,
                       std::vector<u8>&    out) noexcept;

// ──────────────────────────────────────────────────────────────────────────
// xor_delta_decode — inverse of xor_delta_encode. Reads `delta` (the
// RLE-compressed XOR stream), XORs against `baseline`, writes the
// reconstructed snapshot into `out` (resized to `baseline.size()`).
//
// Returns true on success. Returns false if `delta` is truncated mid-run
// (e.g. ends with a lone 0x00 with no run-length byte).
// ──────────────────────────────────────────────────────────────────────────
bool xor_delta_decode(std::span<const u8> baseline,
                      std::span<const u8> delta,
                      std::vector<u8>&    out) noexcept;

}  // namespace psynder::net
