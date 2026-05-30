// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/world/outdoor/TerrainAgents.cpp — see TerrainAgents.h.

#include "world/outdoor/TerrainAgents.h"

#include "world/outdoor/HeightfieldQuery.h"

#include "scene/GxComponents.h"  // TransformWS

namespace psynder::world::outdoor {

void apply_terrain_clamp(scene::World& w, const HeightmapDesc& h) noexcept {
    w.for_each_chunk<scene::TransformWS, GroundClamp>(
        [&](usize n, scene::TransformWS* xf, GroundClamp* gc) {
            for (usize i = 0; i < n; ++i) {
                // The translation column carries world XZ; resample Y from the
                // terrain. prev_mtw is left to whoever moved the entity this tick
                // (e.g. update_agents) so motion interpolation stays correct.
                const f32 wx = xf[i].mtw.m[12];
                const f32 wz = xf[i].mtw.m[14];
                xf[i].mtw.m[13] = terrain_height(h, wx, wz) + gc[i].foot_offset_m;
            }
        });
}

}  // namespace psynder::world::outdoor
