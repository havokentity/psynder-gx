// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/world/outdoor/Splatmap.h — continuous terrain material BLEND WEIGHTS
// for RENDERING. This is the SOFT-blend complement to TerrainMaterial.h: where
// `terrain_material` makes a single HARD pick (grass / rock / snow / sand) used
// by gameplay (footsteps, friction), `terrain_splat` returns HOW MUCH of each
// material to cross-fade at a world point, so the renderer dissolves between
// surfaces over a transition band instead of drawing a hard seam. The four
// weights are normalised to sum to ~1 and feed a texture splat / material
// blend in the shader.
//
// COSMETIC, NOT AUTHORITATIVE: these weights drive *visuals only* — gameplay
// (collision, material friction, footstep cadence) keys off TerrainMaterial's
// hard classification, never these blend weights. Do NOT round a splat weight
// back into a gameplay decision; that is what `terrain_material` is for, and
// duplicating its thresholds here would invite divergence.
//
// Determinism: even though the output is cosmetic, the computation is pure
// ALGEBRA over the lockstep-safe `terrain_slope_updot` (cos of the slope angle)
// and `terrain_height` (bilinear elevation), shaped by polynomial smoothstep
// ramps. There is NO acos / transcendental on the value path, so the same
// serialized terrain yields BIT-IDENTICAL weights across arm64 / x86_64 / MSVC
// under the lane's strict-FP flags. Metric: world X/Z/Y in metres; slope
// thresholds in dimensionless up-dot, elevation thresholds in metres.

#pragma once

#include "world/outdoor/Terrain.h"  // HeightmapDesc

#include "core/Types.h"

namespace psynder::world::outdoor {

// Per-material blend weights at a terrain point. Each is in [0, 1] and the four
// are normalised so `grass + rock + snow + sand` ~ 1 (an all-zero raw result is
// guarded to full grass). The renderer uses these directly as splat weights.
struct SplatWeights {
    f32 grass;  // soil cover on gentle, mid-elevation ground (the default)
    f32 rock;   // bare stone on steep faces (fades in as the slope steepens)
    f32 snow;   // snow cap on high ground (fades in with elevation)
    f32 sand;   // beach / dune on very low ground (fades out with elevation)
};

// Soft transition ranges for the blend ramps (the cosmetic analogue of
// TerrainMaterial's hard MaterialBands thresholds — kept SEPARATE on purpose so
// the visual cross-fade can be tuned without touching the authoritative
// classifier).
//
//   rock_slope_hi / rock_slope_lo — STEEPNESS ramp on the up-dot. Rock fades IN
//       as the up-dot DROPS from `rock_slope_hi` (flatter end, rock weight 0)
//       down to `rock_slope_lo` (steeper end, rock weight 1). hi > lo because a
//       larger up-dot is flatter ground. (e.g. cos(40deg)~0.766 .. cos(55deg)
//       ~0.574.)
//   snow_height_lo / snow_height_hi — ELEVATION ramp. Snow fades IN as height
//       RISES from `snow_height_lo` (snow weight 0) to `snow_height_hi` (snow
//       weight 1), in metres.
//   sand_height_hi — ELEVATION ceiling for sand. Sand is full below it and
//       fades OUT to 0 by `sand_height_hi` (metres), smoothed over the band
//       [sand_height_hi - blend_range_m, sand_height_hi].
//   blend_range_m — width (metres) of the elevation smoothing band used for the
//       sand fade-out edge, so the waterline transition is not a hard step.
struct SplatBands {
    f32 rock_slope_lo;   // up-dot at/below which rock is fully on
    f32 rock_slope_hi;   // up-dot at/above which rock is fully off
    f32 snow_height_lo;  // metres: below this no snow
    f32 snow_height_hi;  // metres: at/above this full snow
    f32 sand_height_hi;  // metres: at/above this no sand
    f32 blend_range_m;   // metres: elevation smoothing width for the sand edge
};

// Sensible defaults for a Battlefield-light outdoor map (metric). These mirror
// the SPIRIT of kDefaultMaterialBands but as soft ramps rather than hard gates:
//   - rock fades in across up-dot 0.766 .. 0.574 (~40deg .. ~55deg of slope),
//     straddling kDefaultMaterialBands' 0.6428 (~50deg) hard rock gate.
//   - snow fades in across 50 m .. 70 m, straddling the 60 m hard snow line.
//   - sand is full at the waterline and fades out by 4 m, straddling the 2 m
//     hard sand band; blend_range_m 4 m widens that elevation edge.
inline constexpr SplatBands kDefaultSplatBands{
    /*rock_slope_lo  =*/0.574f,  // ~cos(55deg)
    /*rock_slope_hi  =*/0.766f,  // ~cos(40deg)
    /*snow_height_lo =*/50.0f,
    /*snow_height_hi =*/70.0f,
    /*sand_height_hi =*/4.0f,
    /*blend_range_m  =*/4.0f,
};

// Clamped cubic smoothstep in [0, 1]: 0 for x <= edge0, 1 for x >= edge1, and
// the Hermite 3t^2 - 2t^3 ease in between (so it is 0.5 exactly at the
// midpoint). When edge0 == edge1 it degenerates to a HARD STEP: 0 for
// x < edge0, 1 for x >= edge0. Polynomial only — bitwise-deterministic.
f32 smoothstep01(f32 edge0, f32 edge1, f32 x) noexcept;

// Continuous material blend weights at world (wx, wz).
//
// MODEL (documented + tested), evaluated from the lockstep-safe up-dot u and
// bilinear height y:
//   1. rock_w  = smoothstep01(b.rock_slope_lo, b.rock_slope_hi, u) inverted, i.e.
//      1 - smoothstep(lo, hi, u): rock grows as the up-dot drops (STEEPER ground
//      => more rock), full rock at/below rock_slope_lo, none at/above
//      rock_slope_hi.
//   2. snow_w  = smoothstep01(snow_height_lo, snow_height_hi, y), GATED by how
//      flat the ground is (* the non-rock fraction (1 - rock_w)): snow only
//      caps ground that is not already a bare steep face — a steep high cliff
//      reads as rock, not snow, matching the hard classifier's priority.
//   3. sand_w  = (1 - snow_w) * (1 - rock_w) * (1 - smoothstep01(
//      sand_height_hi - blend_range_m, sand_height_hi, y)): full at the
//      waterline, fading out by sand_height_hi, and suppressed where rock/snow
//      already dominate.
//   4. grass_w = the REMAINDER, max(0, 1 - rock_w - snow_w - sand_w): soil
//      cover fills whatever the other three do not claim.
// The four raw weights are then NORMALISED to sum to 1; an all-zero result
// (only reachable defensively) is guarded to full grass.
SplatWeights terrain_splat(const HeightmapDesc& h, f32 wx, f32 wz,
                           const SplatBands& b) noexcept;

}  // namespace psynder::world::outdoor
