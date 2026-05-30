// SPDX-License-Identifier: MIT OR Apache-2.0
// Psynder-GX — portable little-endian wire packing for quantized snapshots.
// Lane 18 (net).
//
// SnapshotQuantized.h maps each float `EntityState` to an integer-domain
// `QuantizedState` (position as step counts at a metric resolution, yaw as
// milli-degrees). This header serialises a *set* of those records to a flat
// byte buffer and back, with a wire format that is bit-identical on every
// platform regardless of host endianness.
//
// Wire format (all integers explicit little-endian):
//   header : u32 count                              — number of records
//   record : u32 id, i32 pos[3], i32 yaw_milli      — one QuantizedState
//
// Crucially the integers are appended byte-by-byte in LE order — we never
// `memcpy` a `QuantizedState` struct, so the stream does not depend on host
// byte order or struct padding. A big-endian and a little-endian machine
// produce the exact same bytes for the same input, satisfying the lockstep /
// server-authoritative determinism contract.
//
// Footprint & precision:
//   - Each record is 20 bytes (4 + 12 + 4), the same size as the 20-byte float
//     `EntityState` — the WIN is not the raw size but that every field is an
//     integer the downstream packer can delta- / bit-pack / varint-compress far
//     harder than a raw f32 (small frame-to-frame deltas collapse to a few
//     bits, whereas float bit patterns do not).
//   - Round-trip position error is bounded by `pos_resolution_m / 2` (inherited
//     from `net::quantize`/`dequantize`); yaw within ~0.0005 deg.
//
// Determinism: strict-FP net lane (-fno-fast-math -ffp-contract=off,
// DESIGN-PSYNDER-GX.md §14). The quantize/dequantize stages are pure
// integer/double algebra with no transcendentals; the pack/unpack stages are
// pure integer byte shuffling.

#pragma once

#include "core/Types.h"
#include "net/SnapshotReplication.h"

#include <span>
#include <vector>

namespace psynder::net {

// ──────────────────────────────────────────────────────────────────────────
// pack_quantized — quantize each `EntityState` and serialise the set to `out`.
// `out` is cleared, then written as: a little-endian u32 count followed by one
// 20-byte little-endian record per state (u32 id, i32 pos[3], i32 yaw_milli).
// Records are emitted in the iteration order of `states`, giving a stable,
// tick-reproducible byte stream. Every integer is appended in explicit LE byte
// order — no struct memcpy — so the bytes are identical across platforms.
// ──────────────────────────────────────────────────────────────────────────
void pack_quantized(std::span<const EntityState> states,
                    f32                          pos_resolution_m,
                    std::vector<u8>&             out);

// ──────────────────────────────────────────────────────────────────────────
// unpack_quantized — inverse of `pack_quantized`. Reads the LE count, then that
// many 20-byte records, dequantising each back into an `EntityState` (via
// `dequantize_state`). On success `out` is cleared then filled with the records
// in stream order and the function returns true. Returns false (leaving `out`
// untouched) if `bytes` is truncated or malformed — the header is missing, or
// the declared record count runs past the end of the buffer.
// ──────────────────────────────────────────────────────────────────────────
bool unpack_quantized(std::span<const u8>       bytes,
                      f32                       pos_resolution_m,
                      std::vector<EntityState>& out);

// ──────────────────────────────────────────────────────────────────────────
// packed_size — exact byte size of a packed buffer holding `entity_count`
// records: 4 (the u32 count header) + entity_count * (4 + 12 + 4) =
// 4 + 20 * entity_count. The per-record 20 bytes matches the raw float
// `EntityState` footprint; the bandwidth advantage of the quantized form is
// realised by a downstream delta/bit-packer, not by this fixed-width layout.
// ──────────────────────────────────────────────────────────────────────────
usize packed_size(usize entity_count) noexcept;

}  // namespace psynder::net
