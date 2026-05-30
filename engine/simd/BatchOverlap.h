// SPDX-License-Identifier: MIT
// Psynder — Lane 03 (SIMD). Batch broadphase-overlap kernel.
//
// The inner loop of a broadphase neighbour query: given a query sphere
// (centre + radius) and a batch of candidate points laid out SoA
// (xs[], ys[], zs[]), test which points fall inside the sphere using a
// squared-distance compare (no sqrt). The body processes the SIMD width at
// a time with a scalar tail, and ships a scalar reference that is
// BIT-IDENTICAL to the vector path so determinism is provable.
//
// Why bit-identity holds: the squared distance is computed as
//   d2 = dx*dx + dy*dy + dz*dz
// with PLAIN multiplies and adds in a fixed association order. We
// deliberately do NOT use FMA here — a fused multiply-add rounds the
// intermediate differently from a separate mul+add, which would make the
// SIMD path diverge from the scalar path bit-for-bit. The compare is
// `d2 <= r*r`, the same predicate in both paths. Per-lane this is the
// identical sequence of IEEE-754 single-precision operations, so the mask
// matches the scalar mask exactly for any input, tail included.
//
// §1b perf guardrails: zero per-call heap allocation (the caller owns every
// output buffer), SoA-friendly (the three coordinate streams are read
// linearly), and deterministic (squared distance only, fixed op order).

#pragma once

#include "core/Types.h"

#include <span>

namespace psynder::simd {

// out_inside[i] = 1 when point i is within radius r of (cx,cy,cz), else 0.
// The compare is squared-distance (no sqrt): d2 <= r*r. The number of points
// processed is the minimum of the four spans (xs, ys, zs, out_inside).
// Returns the count of points marked inside. SIMD over the width, scalar tail.
// Zero heap allocation — out_inside is caller-provided. noexcept.
usize sphere_overlap_mask(f32 cx, f32 cy, f32 cz, f32 r,
                          std::span<const f32> xs,
                          std::span<const f32> ys,
                          std::span<const f32> zs,
                          std::span<u8> out_inside) noexcept;

// Bit-identical scalar reference for sphere_overlap_mask. Same signature,
// same arithmetic (dx*dx + dy*dy + dz*dz, compare <= r*r), same op order.
// Used by tests to prove the SIMD path matches scalar bitwise; also the
// fallback the vector path collapses to on the non-width tail.
usize sphere_overlap_scalar(f32 cx, f32 cy, f32 cz, f32 r,
                            std::span<const f32> xs,
                            std::span<const f32> ys,
                            std::span<const f32> zs,
                            std::span<u8> out_inside) noexcept;

// Writes the ascending indices of the inside points into out_indices,
// stopping when out_indices is full (capacity = out_indices.size()). The
// candidate count is the minimum of the xs/ys/zs spans. Returns the number
// of indices written. Zero heap allocation — out_indices is caller-provided.
// The indices it emits are exactly the set bits sphere_overlap_mask would
// produce, in ascending order. noexcept.
usize sphere_overlap_indices(f32 cx, f32 cy, f32 cz, f32 r,
                             std::span<const f32> xs,
                             std::span<const f32> ys,
                             std::span<const f32> zs,
                             std::span<u32> out_indices) noexcept;

}  // namespace psynder::simd
