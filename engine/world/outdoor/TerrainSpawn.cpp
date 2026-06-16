// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/world/outdoor/TerrainSpawn.cpp — see TerrainSpawn.h. Reuses the lane's
// HeightfieldQuery ground-clamp so the chosen spawn sits on exactly the terrain
// data the renderer draws.

#include "world/outdoor/TerrainSpawn.h"

#include "world/outdoor/HeightfieldQuery.h"  // clamp_to_ground

namespace psynder::world::outdoor {

usize select_farthest_spawn(std::span<const math::Vec3> candidates,
                            std::span<const math::Vec3> enemies) noexcept {
    if (candidates.empty() || enemies.empty()) return 0;

    usize best_idx = 0;
    f32 best_min_d2 = -1.0f;  // maximize the nearest-enemy distance

    for (usize i = 0; i < candidates.size(); ++i) {
        const math::Vec3 sp = candidates[i];
        f32 min_d2 = -1.0f;  // nearest enemy (XZ) to this candidate
        for (usize k = 0; k < enemies.size(); ++k) {
            const f32 dx = enemies[k].x - sp.x;
            const f32 dz = enemies[k].z - sp.z;  // XZ only — ignore Y
            const f32 d2 = dx * dx + dz * dz;
            if (min_d2 < 0.0f || d2 < min_d2) min_d2 = d2;
        }
        // Strictly greater => the first (lowest-index) candidate wins ties.
        if (min_d2 > best_min_d2) {
            best_min_d2 = min_d2;
            best_idx = i;
        }
    }

    return best_idx;
}

math::Vec3 pick_terrain_spawn(const HeightmapDesc& h,
                              std::span<const math::Vec3> candidates,
                              std::span<const math::Vec3> enemies,
                              f32 foot_offset) noexcept {
    if (candidates.empty()) return math::Vec3{0.0f, 0.0f, 0.0f};
    const usize idx = select_farthest_spawn(candidates, enemies);
    return clamp_to_ground(h, candidates[idx], foot_offset);
}

}  // namespace psynder::world::outdoor
