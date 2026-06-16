// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/world/outdoor/TerrainMaterial.cpp — see TerrainMaterial.h. Pure
// algebra over the lockstep-safe slope up-dot + bilinear height; no acos /
// transcendental on the value path, so material classification is
// bit-identical across platforms for the same serialized terrain.

#include "world/outdoor/TerrainMaterial.h"

#include "world/outdoor/TerrainSlope.h"     // terrain_slope_updot
#include "world/outdoor/HeightfieldQuery.h"  // terrain_height

namespace psynder::world::outdoor {

SurfaceMaterial terrain_material(const HeightmapDesc& h, f32 wx, f32 wz,
                                 const MaterialBands& b) noexcept {
    // 1) STEEPNESS FIRST: a face too steep to hold soil is bare Rock, whatever
    //    its elevation (a high cliff is rock, not snow). The up-dot is the
    //    lockstep-safe cos-of-slope; rock_max_updot is a precomputed
    //    cos-threshold, so this is a plain algebraic comparison (no acos).
    const f32 updot = terrain_slope_updot(h, wx, wz);
    if (updot < b.rock_max_updot) {
        return SurfaceMaterial::Rock;
    }

    // 2) Otherwise classify by ELEVATION.
    const f32 height = terrain_height(h, wx, wz);
    if (height >= b.snow_min_height_m) {
        return SurfaceMaterial::Snow;
    }
    if (height <= b.sand_max_height_m) {
        return SurfaceMaterial::Sand;
    }
    return SurfaceMaterial::Grass;
}

f32 material_friction(SurfaceMaterial m) noexcept {
    switch (m) {
        case SurfaceMaterial::Grass: return 1.0f;  // firm footing (reference)
        case SurfaceMaterial::Rock:  return 1.0f;  // bare stone, full grip
        case SurfaceMaterial::Snow:  return 0.6f;  // slippery, lowest traction
        case SurfaceMaterial::Sand:  return 0.8f;  // loose, some slip
    }
    return 1.0f;  // defensive default — unreachable for valid enum values
}

}  // namespace psynder::world::outdoor
