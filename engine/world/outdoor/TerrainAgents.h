// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/world/outdoor/TerrainAgents.h — bind ECS movers to the heightfield.
//
// The DOTS agent system (engine/physics/agents) and the player capsule move on
// the XZ plane; this pass snaps their Y onto the outdoor terrain surface each
// tick, so agents / players / props walk over a Battlefield-light heightfield
// instead of a flat floor. Deterministic + order-independent (each entity's
// clamp is a pure function of its XZ and the shared heightmap) — safe to run on
// the lockstep tick after update_agents. Built on HeightfieldQuery, so movers
// collide against exactly the rendered terrain.

#pragma once

#include "world/outdoor/Terrain.h"  // HeightmapDesc

#include "scene/World.h"  // PSYNDER_COMPONENT, scene::World

#include "core/Types.h"

namespace psynder::world::outdoor {

// Tag an entity (with a TransformWS) as terrain-bound: apply_terrain_clamp snaps
// its Y to the terrain surface + foot_offset_m (e.g. a capsule's foot-to-origin
// height, or 0 for a foot-origin transform).
PSYNDER_COMPONENT(GroundClamp) {
    f32 foot_offset_m;
};
static_assert(sizeof(GroundClamp) == 4, "GroundClamp layout frozen");

// Snap every (TransformWS + GroundClamp) entity's world Y to the heightfield
// surface at its XZ plus its foot offset. Order-independent + deterministic.
void apply_terrain_clamp(scene::World& w, const HeightmapDesc& h) noexcept;

}  // namespace psynder::world::outdoor
