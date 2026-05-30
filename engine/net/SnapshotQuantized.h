// SPDX-License-Identifier: MIT OR Apache-2.0
// Psynder-GX — bandwidth-efficient quantized snapshot states. Lane 18 (net).
//
// The float `EntityState` (SnapshotReplication.h) ships 20 bytes per entity:
// u32 id + 3*f32 pos + f32 yaw_deg. For a homogeneous mass-agent stream that
// raw float payload is wasteful — world-space positions only need a fixed
// spatial resolution and facing only needs sub-degree precision. This header
// re-expresses a single entity record as a `QuantizedState`: position as an
// integer step count at a metric resolution (via the deterministic
// `net::quantize`) and yaw as an integer count of milli-degrees.
//
// This is purely additive: the float `EntityState` and the existing delta
// codec (SnapshotDelta.h / SnapshotReplication.h) are untouched. The quantized
// form is a drop-in upstream stage — quantize each `EntityState` before it
// enters the codec, dequantize after `apply_delta` rebuilds the set — and a
// bandwidth probe (`quantized_wire_bytes`) for measuring the saving.
//
// Precision / round-trip bounds:
//   - Position: each component round-trips within `pos_resolution_m / 2`
//     (the half-step bound inherited from `net::quantize`/`dequantize`).
//   - Yaw: stored as round(yaw_deg * 1000) milli-degrees, so the reconstructed
//     yaw is within 0.0005 deg (half of one milli-degree step) of the input.
//
// Determinism: this is the strict-FP net lane (-fno-fast-math
// -ffp-contract=off, DESIGN-PSYNDER-GX.md §14). Both directions are pure
// integer/double algebra with NO transcendentals — the position mapping defers
// to `net::quantize` (divide + floor in double), and the yaw mapping is a
// multiply + floor(x + 0.5) in double. For finite inputs the result is
// bit-identical across platforms, satisfying lockstep / server-authoritative
// reconciliation.

#pragma once

#include "core/Types.h"
#include "net/SnapshotReplication.h"

namespace psynder::net {

// ──────────────────────────────────────────────────────────────────────────
// QuantizedState — the integer-domain mirror of `EntityState`. POD, trivially
// copyable, SoA-friendly. `pos` is the per-component integer step count at the
// position resolution; `yaw_milli` is the facing in milli-degrees (1/1000 of a
// degree per step). 20 bytes, same footprint as the float record but every
// field is an integer the wire packer can bit-pack / varint-compress far
// harder than a raw f32 (small frame-to-frame deltas collapse to a few bits).
// ──────────────────────────────────────────────────────────────────────────
struct QuantizedState {
    u32 id        = 0;
    i32 pos[3]    = {0, 0, 0};
    i32 yaw_milli = 0;
};

static_assert(std::is_trivially_copyable_v<QuantizedState>,
              "QuantizedState must be trivially copyable for the DOTS/SoA contract");

// ──────────────────────────────────────────────────────────────────────────
// quantize_state — map a float `EntityState` to its integer `QuantizedState`.
// `id` is copied verbatim; each position component is quantized with
// `net::quantize(pos, pos_resolution_m)`; yaw becomes round(yaw_deg * 1000)
// milli-degrees via floor(x + 0.5) evaluated in double (matching the half-up
// tie behaviour of `net::quantize`). Deterministic, no transcendentals.
// ──────────────────────────────────────────────────────────────────────────
QuantizedState quantize_state(const EntityState& s, f32 pos_resolution_m) noexcept;

// ──────────────────────────────────────────────────────────────────────────
// dequantize_state — inverse of `quantize_state`. `id` is copied; each
// position component is reconstructed with `net::dequantize`; yaw_deg is
// `yaw_milli / 1000`. Round-trip error is bounded by `pos_resolution_m / 2` on
// position and 0.0005 deg on yaw.
// ──────────────────────────────────────────────────────────────────────────
EntityState dequantize_state(const QuantizedState& q, f32 pos_resolution_m) noexcept;

// ──────────────────────────────────────────────────────────────────────────
// quantized_wire_bytes — sizeof(QuantizedState), a bandwidth probe for
// comparing the quantized record against the 20-byte float `EntityState`. The
// raw struct sizes match; the real saving comes from the wire packer
// bit-packing the integers, but this exposes the per-record footprint callers
// can budget against.
// ──────────────────────────────────────────────────────────────────────────
usize quantized_wire_bytes() noexcept;

}  // namespace psynder::net
