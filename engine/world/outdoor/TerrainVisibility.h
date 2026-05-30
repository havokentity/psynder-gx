// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/world/outdoor/TerrainVisibility.h — deterministic terrain
// line-of-sight / visibility queries on the heightfield, layered on
// HeightfieldQuery's bilinear `terrain_height`. The tactical-AI question: can a
// bot at A see / shoot a target at B, or do the hills between them occlude the
// straight segment? Used to gate aggro / target selection / fire decisions on a
// Battlefield-light outdoor map.
//
// Determinism: both queries are pure ALGEBRAIC operations (+ - * /) over
// `terrain_height` — no transcendentals, no RNG, no allocation. The segment is
// marched at a sample count derived DETERMINISTICALLY from the XZ length and the
// texel spacing (an integer step count), so the same inputs produce a
// bit-identical result across arm64 / x86_64 / MSVC under the lane's strict-FP
// flags, and the queries are safe on the lockstep tick. Metric: world X/Z/Y in
// metres.
//
// Sampling rule (shared by both functions below): the interior of A->B is
// sampled at N evenly-spaced points P = lerp(a, b, t), t in (0, 1). N is chosen
// so the step along XZ is at most ~one texel (`spacing`) — `N = ceil(xz_len /
// spacing)`, clamped to a minimum of `kMinSamples` so even very short or
// near-vertical segments still probe the surface. This guarantees a one-texel
// hill on the line cannot be stepped over. Endpoints are NOT tested: the caller
// is assumed to have placed the eye / target above the surface already.

#pragma once

#include "world/outdoor/Terrain.h"  // HeightmapDesc

#include "math/Math.h"
#include "core/Types.h"

namespace psynder::world::outdoor {

// True when the terrain does NOT occlude the straight segment A->B, i.e. every
// interior sample point clears the surface by at least `clearance_m`. Marches
// A->B per the sampling rule documented in the file header; at each interior
// sample P requires `P.y >= terrain_height(h, P.x, P.z) + clearance_m`. If any
// interior sample dips below, the line is OCCLUDED and this returns false.
// A zero-length segment (a == b) is trivially visible (returns true).
// `clearance_m` is an optional ground-clearance margin (the line must clear the
// terrain by at least this much) — default 0. Pure algebra — lockstep-safe.
bool terrain_line_of_sight(const HeightmapDesc& h, math::Vec3 a, math::Vec3 b,
                           f32 clearance_m = 0.0f) noexcept;

// The MINIMUM signed clearance (P.y - terrain_height at P) over the sampled
// interior points of A->B, using the SAME sampling as terrain_line_of_sight:
//   > 0  => the line clears the terrain everywhere by that margin (visible),
//   <= 0 => it dips into / below the surface (occluded) by that depth.
// So `terrain_line_of_sight(h, a, b, c)` is equivalent to
// `terrain_los_clearance(h, a, b) >= c` (modulo the discrete sampling). A
// zero-length segment has no interior samples and reports +infinity-like "fully
// clear"; see the .cpp for the sentinel. Pure algebra — lockstep-safe.
f32 terrain_los_clearance(const HeightmapDesc& h, math::Vec3 a,
                          math::Vec3 b) noexcept;

}  // namespace psynder::world::outdoor
