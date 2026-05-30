// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/world/outdoor/Splatmap.cpp — see Splatmap.h. SOFT material BLEND
// weights for rendering, the cross-fading complement to TerrainMaterial's hard
// pick. Pure algebra over the lockstep-safe slope up-dot + bilinear height,
// shaped by polynomial smoothstep ramps; no acos / transcendental on the value
// path, so the same serialized terrain yields bit-identical weights across
// platforms (the output is cosmetic, but it stays deterministic regardless).

#include "world/outdoor/Splatmap.h"

#include "world/outdoor/TerrainSlope.h"      // terrain_slope_updot
#include "world/outdoor/HeightfieldQuery.h"  // terrain_height

namespace psynder::world::outdoor {

namespace {

// Plain clamp to [0, 1] without pulling in <algorithm>; pure comparisons keep
// the value path strict-FP deterministic.
inline f32 clamp01(f32 v) noexcept {
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

}  // namespace

f32 smoothstep01(f32 edge0, f32 edge1, f32 x) noexcept {
    // Degenerate band => hard step: 0 below the edge, 1 at/above it. Guarding on
    // equality (rather than dividing by zero) keeps this branch deterministic.
    if (edge0 == edge1) {
        return (x < edge0) ? 0.0f : 1.0f;
    }
    const f32 t = clamp01((x - edge0) / (edge1 - edge0));
    // Hermite ease 3t^2 - 2t^3 — polynomial, so bitwise-stable cross-platform.
    return t * t * (3.0f - 2.0f * t);
}

SplatWeights terrain_splat(const HeightmapDesc& h, f32 wx, f32 wz,
                           const SplatBands& b) noexcept {
    const f32 u = terrain_slope_updot(h, wx, wz);  // 1 = flat, -> 0 steeper
    const f32 y = terrain_height(h, wx, wz);       // metres

    // 1) ROCK from steepness. smoothstep(lo, hi, u) is 1 on flat ground and 0
    //    on the steepest; invert so rock grows as the ground steepens (the
    //    up-dot drops). Full rock at/below rock_slope_lo, none at/above
    //    rock_slope_hi.
    const f32 rock_w = 1.0f - smoothstep01(b.rock_slope_lo, b.rock_slope_hi, u);

    // 2) SNOW from elevation, gated by the non-rock fraction so a steep high
    //    cliff reads as rock, not snow (mirrors the hard classifier's priority).
    const f32 snow_raw = smoothstep01(b.snow_height_lo, b.snow_height_hi, y);
    const f32 snow_w = snow_raw * (1.0f - rock_w);

    // 3) SAND from low elevation: full at the waterline, fading out by
    //    sand_height_hi over a blend_range_m-wide band, and suppressed where
    //    rock or snow already dominate.
    const f32 sand_lo = b.sand_height_hi - b.blend_range_m;
    const f32 sand_falloff = 1.0f - smoothstep01(sand_lo, b.sand_height_hi, y);
    const f32 sand_w = sand_falloff * (1.0f - rock_w) * (1.0f - snow_w);

    // 4) GRASS is the remainder — soil fills whatever rock/snow/sand leave.
    f32 grass_w = 1.0f - rock_w - snow_w - sand_w;
    if (grass_w < 0.0f) grass_w = 0.0f;  // numeric guard; sum still normalised

    // NORMALISE to sum to 1. All-zero is only reachable defensively (every ramp
    // would have to vanish); guard it to full grass.
    const f32 sum = grass_w + rock_w + snow_w + sand_w;
    if (sum <= 0.0f) {
        return SplatWeights{/*grass=*/1.0f, /*rock=*/0.0f, /*snow=*/0.0f,
                            /*sand=*/0.0f};
    }
    const f32 inv = 1.0f / sum;
    return SplatWeights{
        /*grass=*/grass_w * inv,
        /*rock =*/rock_w * inv,
        /*snow =*/snow_w * inv,
        /*sand =*/sand_w * inv,
    };
}

}  // namespace psynder::world::outdoor
