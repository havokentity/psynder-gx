// SPDX-License-Identifier: MIT OR Apache-2.0
// Psynder-GX — deterministic fixed-point quantization. Lane 18 (net).
//
// Bandwidth-efficient netcode snapshots ship world-space positions as integer
// step counts at a fixed spatial resolution rather than raw f32. At 1 mm
// resolution a ±32 km playfield fits inside an i32, while the wire payload can
// later pack the integer with far fewer bits than a 32-bit float (and varints /
// bit-packing compress small deltas hard). This header only does the
// float↔integer mapping; the wire packing lives in the delta codec.
//
// Determinism: this is the strict-FP net lane (-fno-fast-math
// -ffp-contract=off, DESIGN-PSYNDER-GX.md §14). The mapping is pure algebra +
// integer ops with NO transcendentals — the divide+floor is done in double and
// is bit-identical across platforms for finite inputs, so two machines quantize
// the same position to the same integer. That is a hard requirement for
// lockstep / server-authoritative reconciliation.
//
// Resolution: `resolution_m` is the size of one integer step in metres, e.g.
// 0.001 for 1 mm or 0.01 for 1 cm (1 world unit = 1 metre). The round-trip
// error |dequantize(quantize(v)) - v| is bounded by resolution_m / 2.

#pragma once

#include "core/Types.h"

namespace psynder::net {

// ──────────────────────────────────────────────────────────────────────────
// quantize — map a metric `value` to the round-to-nearest integer count of
// `resolution_m` steps. Rounding is deterministic: floor(value/res + 0.5),
// evaluated in double to reduce bias, then narrowed to i32. If `resolution_m`
// is <= 0 (invalid), returns 0.
//
// Note the half-up tie behaviour of floor(x + 0.5): 2.5 → 3, but -2.5 → -2
// (floor(-2.0)), i.e. ties round toward +∞, not away from zero. This is
// intentional and stable across platforms.
// ──────────────────────────────────────────────────────────────────────────
i32 quantize(f32 value, f32 resolution_m) noexcept;

// ──────────────────────────────────────────────────────────────────────────
// dequantize — inverse of quantize: turn an integer step count back into a
// metric value, `q * resolution_m`. The product is formed in double then
// narrowed to f32. Reconstruction error is bounded by resolution_m / 2.
// ──────────────────────────────────────────────────────────────────────────
f32 dequantize(i32 q, f32 resolution_m) noexcept;

// ──────────────────────────────────────────────────────────────────────────
// quantize_pos / dequantize_pos — per-component quantize/dequantize of a
// world-space position (f32[3] ↔ i32[3]) at a uniform resolution.
// ──────────────────────────────────────────────────────────────────────────
void quantize_pos(const f32 in[3], f32 resolution_m, i32 out[3]) noexcept;
void dequantize_pos(const i32 in[3], f32 resolution_m, f32 out[3]) noexcept;

}  // namespace psynder::net
