// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/world/outdoor/TerrainSlope.cpp — see TerrainSlope.h. Built directly on
// HeightfieldQuery's `terrain_normal` so slope / walkability agree exactly with
// the surface normal the rest of the gameplay layer collides against.

#include "world/outdoor/TerrainSlope.h"

#include "world/outdoor/HeightfieldQuery.h"  // terrain_normal

#include <cmath>

namespace psynder::world::outdoor {

f32 terrain_slope_updot(const HeightmapDesc& h, f32 wx, f32 wz) noexcept {
    // The normal is unit length with +Y up on flat ground, so its Y component
    // is cos(slope angle): 1 flat, -> 0 as the surface goes vertical.
    return terrain_normal(h, wx, wz).y;
}

f32 terrain_slope_radians(const HeightmapDesc& h, f32 wx, f32 wz) noexcept {
    f32 c = terrain_slope_updot(h, wx, wz);
    // Clamp guards against tiny FP overshoot outside acos()'s [-1, 1] domain.
    if (c < -1.0f) c = -1.0f;
    if (c > 1.0f) c = 1.0f;
    return std::acos(c);  // transcendental: query/debug only (see header).
}

bool terrain_walkable(const HeightmapDesc& h, f32 wx, f32 wz,
                      f32 min_updot) noexcept {
    return terrain_slope_updot(h, wx, wz) >= min_updot;
}

}  // namespace psynder::world::outdoor
