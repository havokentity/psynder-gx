// SPDX-License-Identifier: MIT OR Apache-2.0
// Psynder-GX — quantized snapshot DELTA codec. Lane 18 (net).
//
// SnapshotPack.h (the full quantized pack) writes EVERY record EVERY tick; its
// header explicitly defers the bandwidth win to "a downstream delta packer".
// This header IS that packer. Against a `prev` baseline it emits only what
// changed in `curr` — a removed-id list plus the new/moved records — so a
// steady-state stream where most entities are still collapses to a few bytes
// instead of one 20-byte record per entity.
//
// Wire format (all integers explicit little-endian, identical LE convention to
// SnapshotPack.cpp — bytes shifted out one at a time, never a struct memcpy, so
// the stream is bit-identical on every platform regardless of host endianness):
//   removed : u32 removed_count, then removed_count * u32 id   — ids in prev
//             whose entity is absent from curr
//   changed : u32 changed_count, then changed_count * 20-byte record
//             (u32 id, i32 pos[3], i32 yaw_milli)              — each curr
//             entity that is NEW (id not in prev) OR whose QuantizedState
//             differs from its prev counterpart. Unchanged entities are omitted.
//
// Both lists are emitted in ASCENDING id order so the byte stream is
// reproducible regardless of the input ordering of `prev` / `curr`.
//
// "Changed" is decided in the QUANTIZED domain (compare the integer fields, not
// the floats): an entity that moved less than one quantization step quantizes
// to the same record and is omitted, and `apply` still reconstructs the
// baseline value for it. Bit-exact integer compare — lockstep determinism
// forbids float epsilon fuzz.
//
// Determinism: strict-FP net lane (-fno-fast-math -ffp-contract=off,
// DESIGN-PSYNDER-GX.md §14). Every stage here is integer / byte algebra except
// the quantize/dequantize steps, which are delegated to SnapshotQuantized — no
// transcendentals, no RNG, no per-call heap growth beyond clearing/reserving
// the caller-owned output vector (plus small local scratch).

#pragma once

#include "core/Types.h"
#include "net/SnapshotReplication.h"

#include <span>
#include <vector>

namespace psynder::net {

// ──────────────────────────────────────────────────────────────────────────
// pack_quantized_delta — serialise a delta of `curr` against `prev` to `out`.
// Both state sets are quantized (via `quantize_state`) at `pos_resolution_m`,
// then `out` is cleared and written as: the removed list (u32 count + that many
// u32 ids — ids present in prev but absent in curr) followed by the changed list
// (u32 count + that many 20-byte records — curr entities that are new or whose
// quantized record differs from prev). Both lists are in ascending-id order.
// Every integer is appended in explicit LE byte order — no struct memcpy — so
// the bytes are identical across platforms.
// ──────────────────────────────────────────────────────────────────────────
void pack_quantized_delta(std::span<const EntityState> prev,
                          std::span<const EntityState> curr,
                          f32                          pos_resolution_m,
                          std::vector<u8>&             out);

// ──────────────────────────────────────────────────────────────────────────
// apply_quantized_delta — reconstruct `curr` from `prev` + the delta `bytes`.
// Starts from the quantized `prev` set, drops the removed ids, overlays the
// changed/added records, then dequantizes each surviving record into `out`
// (cleared first), emitted in ASCENDING id order. Returns true on success.
// Returns false (leaving `out` untouched) if `bytes` is truncated or malformed
// — a header is missing, or a declared count runs past the end of the buffer.
// ──────────────────────────────────────────────────────────────────────────
bool apply_quantized_delta(std::span<const EntityState> prev,
                           std::span<const u8>          bytes,
                           f32                          pos_resolution_m,
                           std::vector<EntityState>&    out);

// ──────────────────────────────────────────────────────────────────────────
// delta_size — exact byte size of a delta carrying `removed_count` removed ids
// and `changed_count` changed records: 4 (removed count) + removed_count * 4 +
// 4 (changed count) + changed_count * 20. Callers use this to reserve `out`
// once so the steady-state path never reallocates.
// ──────────────────────────────────────────────────────────────────────────
usize delta_size(usize removed_count, usize changed_count) noexcept;

}  // namespace psynder::net
