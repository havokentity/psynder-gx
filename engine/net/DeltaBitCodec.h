// SPDX-License-Identifier: MIT OR Apache-2.0
// Psynder-GX — BIT-LEVEL quantized snapshot DELTA codec. Lane 18 (net).
//
// SnapshotPackDelta.h ships a delta of `curr` against `prev` BYTE-granularly:
// each changed entity costs a fixed 20-byte record (u32 id + i32 pos[3] +
// i32 yaw_milli) regardless of how little it moved. But once the floats have
// been collapsed to integers, the real bandwidth win SnapshotPack / Metrics
// keep promising is SUB-BYTE: a per-tick position delta of a few centimetres at
// 1 mm resolution is a value of ±tens, which zig-zags to a code that fits in a
// handful of bits, not 32. DeltaBitCodec IS that tighter complement — the same
// delta semantics as SnapshotPackDelta (removed-id list + changed records, both
// ASCENDING id) but BIT-PACKED via BitPacker, so a steady-state stream where
// entities drift slowly collapses to a few bytes total.
//
// Wire format (all via BitWriter, LSB-first within each byte — see BitPacker.h;
// the writer is flushed at the end so the trailing partial byte is zero-padded).
// Every count and id is a fixed-width u32; every per-field delta is a zigzag of
// (curr - prev) carried in a self-describing variable-width field:
//
//   removed section:
//     [32b] removed_count
//     removed_count *  [32b] id            — ids in prev absent from curr, ASC
//
//   changed section:
//     [32b] changed_count
//     changed_count * record, in ASCENDING id order:
//       [32b] id                            — the entity's id
//       field x : [6b] wx, [wx b] zz_x      — zigzag(curr.pos[0] - prev.pos[0])
//       field y : [6b] wy, [wy b] zz_y      — zigzag(curr.pos[1] - prev.pos[1])
//       field z : [6b] wz, [wz b] zz_z      — zigzag(curr.pos[2] - prev.pos[2])
//       field w : [6b] ww, [ww b] zz_yaw    — zigzag(curr.yaw_milli - prev.yaw)
//
//   For each field: the 6-bit header `w` is bits_needed(zz) — the minimal width
//   that holds the zigzag code (0..63, ample for a 32-bit field's <=33-bit code,
//   though steady deltas use only a few). The reader pulls `w` then `w` bits and
//   zigzag-decodes back to the signed delta. A field that did not change has
//   zz == 0, so w == 0 and the field costs exactly 6 bits and no payload. A NEW
//   entity (id absent from prev) deltas against an all-zero prev record, so each
//   field carries its full quantized value — correct, just not as tight.
//
// "Changed" is decided in the QUANTIZED domain (compare the integer fields, not
// the floats), exactly as SnapshotPackDelta: an entity that moved less than one
// quantization step quantizes to the same record and is omitted, and `decode`
// still reconstructs the baseline value for it. Bit-exact integer compare —
// lockstep determinism forbids float epsilon fuzz.
//
// Determinism: strict-FP net lane (-fno-fast-math -ffp-contract=off,
// DESIGN-PSYNDER-GX.md §14). Every stage here is pure integer / bit algebra
// except the delegated quantize/dequantize steps — no transcendentals, no RNG,
// no per-call heap growth beyond the caller-owned output vector (plus small
// id-sorted local scratch). Both sides are emitted in ASCENDING id order, so the
// same inputs always produce a byte-identical `out`.

#pragma once

#include "core/Types.h"
#include "net/SnapshotReplication.h"

#include <span>
#include <vector>

namespace psynder::net {

// ──────────────────────────────────────────────────────────────────────────
// encode_bit_delta — serialise a BIT-PACKED delta of `curr` against `prev` to
// `out`. Both state sets are quantized (via `quantize_state`) at
// `pos_resolution_m`, then `out` is cleared and written via a BitWriter as the
// removed list (count + ids — ids present in prev but absent in curr) followed
// by the changed list (count + records — curr entities that are new or whose
// quantized record differs from prev). Each changed record carries its id plus
// the four per-field zigzag(curr - prev_or_0) deltas, each in a self-describing
// 6-bit-width-prefixed variable field (see the header banner). Both lists are in
// ascending-id order; the writer is flushed so `out` is byte-aligned. Identical
// inputs produce a byte-identical `out`.
// ──────────────────────────────────────────────────────────────────────────
void encode_bit_delta(std::span<const EntityState> prev,
                      std::span<const EntityState> curr,
                      f32                          pos_resolution_m,
                      std::vector<u8>&             out);

// ──────────────────────────────────────────────────────────────────────────
// decode_bit_delta — reconstruct `curr` from `prev` + the bit-delta `bytes`.
// Starts from the quantized `prev` set, drops the removed ids, applies each
// changed record's per-field deltas onto the baseline quantized fields (zero
// baseline for a new id), then dequantizes each surviving record into `out`
// (cleared first), emitted in ASCENDING id order. Returns true on success.
// Returns false (leaving `out` UNTOUCHED) if the bit reader overruns the buffer
// at any point — a truncated/malformed payload trips BitReader::ok().
// ──────────────────────────────────────────────────────────────────────────
bool decode_bit_delta(std::span<const EntityState> prev,
                      std::span<const u8>          bytes,
                      f32                          pos_resolution_m,
                      std::vector<EntityState>&    out);

}  // namespace psynder::net
