// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/world/outdoor/TerrainMaterial.h — deterministic terrain SURFACE
// MATERIAL classification on the heightfield. Given a world point, decide what
// the ground is MADE OF (grass / rock / snow / sand) from its STEEPNESS and its
// ELEVATION: steep ground is bare rock (a cliff), high ground is snow-capped,
// very low ground is sand (beach/dune), and the flat mid-elevation default is
// grass. The result drives footstep SOUNDS, visual material BLENDS, and
// movement FRICTION.
//
// Determinism: classification is pure ALGEBRA over the lockstep-safe
// `terrain_slope_updot` (the surface-normal Y, i.e. cos of the slope angle) and
// `terrain_height` (bilinear elevation) — only comparisons against the
// caller's MaterialBands thresholds. NO acos / transcendental on the value
// path, so the same serialized terrain yields BIT-IDENTICAL materials across
// arm64 / x86_64 / MSVC under the lane's strict-FP flags. That parity is what
// lets gameplay derived from the material (e.g. footstep cadence keyed off
// `material_friction`) agree across lockstep peers. Metric: world X/Z/Y in
// metres; thresholds in metres / dimensionless up-dot.

#pragma once

#include "world/outdoor/Terrain.h"  // HeightmapDesc

#include "core/Types.h"

namespace psynder::world::outdoor {

// Surface material at a terrain point. Stable u32 values: serialized into
// footstep / blend tables and compared across peers, so do NOT reorder.
enum class SurfaceMaterial : u32 {
    Grass = 0,  // flat, mid-elevation ground — the default
    Rock  = 1,  // too steep for soil to hold — a bare cliff face
    Snow  = 2,  // high ground — snow line and above
    Sand  = 3,  // very low ground — beach / dune
};

// Classification thresholds. Tune per map / biome.
//   rock_max_updot  — STEEPNESS gate: when the surface up-dot (cos of the slope
//                     angle, 1.0 = flat) is BELOW this, the ground is too steep
//                     to hold soil and classifies as Rock regardless of height.
//                     This is a precomputed cos-threshold (the lockstep-safe
//                     form), NOT an angle — compare the up-dot directly.
//   snow_min_height_m — ELEVATION gate: ground at or ABOVE this height (metres)
//                       is Snow.
//   sand_max_height_m — ELEVATION gate: ground at or BELOW this height (metres)
//                       is Sand.
struct MaterialBands {
    f32 rock_max_updot;     // updot < this  -> Rock  (e.g. cos(50deg) ~ 0.643)
    f32 snow_min_height_m;  // height >= this -> Snow
    f32 sand_max_height_m;  // height <= this -> Sand
};

// Sensible defaults for a Battlefield-light outdoor map (metric):
//   - rock_max_updot 0.6428 ~ cos(50deg): faces steeper than ~50deg are bare
//     rock; gentler slopes keep their soil cover.
//   - snow_min_height_m 60 m: a snow line at 60 m elevation.
//   - sand_max_height_m 2 m: the beach / waterline band at or below 2 m.
inline constexpr MaterialBands kDefaultMaterialBands{
    /*rock_max_updot   =*/0.6428f,
    /*snow_min_height_m=*/60.0f,
    /*sand_max_height_m=*/2.0f,
};

// Classify the surface at world (wx, wz) into a SurfaceMaterial.
//
// PRIORITY (documented + tested):
//   1. STEEPNESS FIRST. If terrain_slope_updot(h, wx, wz) < b.rock_max_updot
//      the face is too steep to hold soil -> Rock, regardless of elevation. A
//      steep HIGH slope is therefore Rock (a bare cliff), NOT Snow.
//   2. Otherwise by ELEVATION, using terrain_height(h, wx, wz):
//        height >= b.snow_min_height_m  -> Snow
//        height <= b.sand_max_height_m  -> Sand
//        else                           -> Grass
//
// Pure algebra over the lockstep-safe up-dot + height queries (no acos) — the
// same serialized terrain gives bit-identical results across platforms.
SurfaceMaterial terrain_material(const HeightmapDesc& h, f32 wx, f32 wz,
                                 const MaterialBands& b) noexcept;

// Movement-friction MULTIPLIER for a material (dimensionless, applied to base
// ground traction): 1.0 is full grip, < 1.0 is slippery. Documented values:
//   Grass 1.0 — firm footing (reference)
//   Rock  1.0 — bare stone, full grip
//   Snow  0.6 — slippery, the lowest traction
//   Sand  0.8 — loose, some slip
// Pure table lookup — trivially deterministic.
f32 material_friction(SurfaceMaterial m) noexcept;

}  // namespace psynder::world::outdoor
