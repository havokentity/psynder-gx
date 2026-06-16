// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/world/outdoor/SpawnValidation.cpp — see SpawnValidation.h. Reuses the
// lane's lockstep-safe terrain_walkable (slope gate) and clamp_to_ground
// (surface snap) so a validated spawn sits on exactly the terrain data the
// renderer draws, with no transcendental on the path.

#include "world/outdoor/SpawnValidation.h"

#include "world/outdoor/TerrainSlope.h"      // terrain_walkable
#include "world/outdoor/HeightfieldQuery.h"  // clamp_to_ground

namespace psynder::world::outdoor {

void filter_walkable_spawns(const HeightmapDesc& h,
                            std::span<const math::Vec3> candidates,
                            f32 min_updot,
                            std::vector<usize>& out_indices) noexcept {
    out_indices.clear();
    // Ascending scan => out_indices is emitted in ascending candidate order.
    for (usize i = 0; i < candidates.size(); ++i) {
        const math::Vec3 c = candidates[i];
        if (terrain_walkable(h, c.x, c.z, min_updot)) out_indices.push_back(i);
    }
}

void clamp_walkable_spawns(const HeightmapDesc& h,
                           std::span<const math::Vec3> candidates,
                           f32 min_updot, f32 foot_offset,
                           std::vector<math::Vec3>& out_points) noexcept {
    out_points.clear();
    for (usize i = 0; i < candidates.size(); ++i) {
        const math::Vec3 c = candidates[i];
        if (terrain_walkable(h, c.x, c.z, min_updot))
            out_points.push_back(clamp_to_ground(h, c, foot_offset));
    }
}

bool any_walkable_spawn(const HeightmapDesc& h,
                        std::span<const math::Vec3> candidates,
                        f32 min_updot) noexcept {
    for (usize i = 0; i < candidates.size(); ++i) {
        const math::Vec3 c = candidates[i];
        if (terrain_walkable(h, c.x, c.z, min_updot)) return true;
    }
    return false;
}

usize first_walkable_index(const HeightmapDesc& h,
                           std::span<const math::Vec3> candidates,
                           f32 min_updot) noexcept {
    for (usize i = 0; i < candidates.size(); ++i) {
        const math::Vec3 c = candidates[i];
        if (terrain_walkable(h, c.x, c.z, min_updot)) return i;
    }
    return candidates.size();  // sentinel: none walkable
}

}  // namespace psynder::world::outdoor
