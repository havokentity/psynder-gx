// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/world/outdoor/TerrainVisibility.cpp — see TerrainVisibility.h. Built
// directly on HeightfieldQuery's bilinear `terrain_height` so the line-of-sight
// test agrees exactly with the surface the rest of the gameplay layer queries.

#include "world/outdoor/TerrainVisibility.h"

#include "world/outdoor/HeightfieldQuery.h"  // terrain_height

#include <cmath>  // std::ceil, std::sqrt — algebraic use only (see note)

namespace psynder::world::outdoor {

namespace {

// Minimum interior sample count: even a very short or near-vertical segment
// (tiny XZ length) still probes the surface this many times before reporting
// clear. Keeps the query honest when XZ length < spacing.
constexpr u32 kMinSamples = 4u;

// Sentinel "fully clear" clearance for a zero-length segment (no interior to
// sample). Large but finite so callers can still compare it against any sane
// margin; chosen well beyond any plausible map height (metres).
constexpr f32 kClearSentinel = 1.0e9f;

// Shared marcher: the MINIMUM signed clearance (P.y - terrain_height at P) over
// the interior samples of A->B. Both public queries funnel through this so the
// documented equivalence `los(a,b,c) == (clearance(a,b) >= c)` holds exactly.
//
// Determinism: the sample count is an integer derived from the XZ length and
// the texel spacing; every per-sample op is +-*/ over `terrain_height`. No
// transcendentals on the value path (std::ceil/std::sqrt below are used only to
// size the integer loop, not to compute the returned clearance), no RNG, no
// allocation — bit-identical across platforms under strict-FP.
f32 min_interior_clearance(const HeightmapDesc& h, math::Vec3 a,
                           math::Vec3 b) noexcept {
    const f32 dx = b.x - a.x;
    const f32 dz = b.z - a.z;
    const f32 xz_len2 = dx * dx + dz * dz;

    // Zero-length (degenerate) segment: no interior to occlude — fully clear.
    if (xz_len2 <= 0.0f) return kClearSentinel;

    // Choose N so the XZ step between samples is at most ~one texel: a one-texel
    // hill on the line then cannot be skipped. `spacing` is metres per texel; a
    // non-positive spacing falls back to one metre so we never divide by zero.
    const f32 xz_len = std::sqrt(xz_len2);
    const f32 spacing = (h.spacing > 0.0f) ? h.spacing : 1.0f;
    f32 nf = std::ceil(xz_len / spacing);
    if (nf < static_cast<f32>(kMinSamples)) nf = static_cast<f32>(kMinSamples);
    const u32 n = static_cast<u32>(nf);

    // March the interior at t = i/(n+1) for i in [1, n] (endpoints excluded —
    // the caller placed eye/target above the surface). Track the worst (lowest)
    // signed clearance: P.y - terrain_height(P.x, P.z).
    f32 worst = kClearSentinel;
    const f32 inv = 1.0f / static_cast<f32>(n + 1u);
    for (u32 i = 1u; i <= n; ++i) {
        const f32 t = static_cast<f32>(i) * inv;
        const f32 px = a.x + (b.x - a.x) * t;
        const f32 py = a.y + (b.y - a.y) * t;
        const f32 pz = a.z + (b.z - a.z) * t;
        const f32 clearance = py - terrain_height(h, px, pz);
        if (clearance < worst) worst = clearance;
    }
    return worst;
}

}  // namespace

bool terrain_line_of_sight(const HeightmapDesc& h, math::Vec3 a, math::Vec3 b,
                           f32 clearance_m) noexcept {
    // Visible iff the worst-case interior sample still clears by the margin.
    return min_interior_clearance(h, a, b) >= clearance_m;
}

f32 terrain_los_clearance(const HeightmapDesc& h, math::Vec3 a,
                          math::Vec3 b) noexcept {
    return min_interior_clearance(h, a, b);
}

}  // namespace psynder::world::outdoor
