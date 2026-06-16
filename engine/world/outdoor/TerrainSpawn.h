// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/world/outdoor/TerrainSpawn.h — deterministic terrain-aware spawn-point
// selection for the Battlefield-light outdoor map. Picks the candidate spawn
// FARTHEST from any living enemy (XZ distance only) and clamps it onto the
// terrain surface via the lane's HeightfieldQuery.
//
// Determinism: pure algebra (+ - * /) over plain math::Vec3 spans — no
// transcendentals, no allocation, no RNG. Safe on the lockstep tick under the
// lane's strict-FP flags. Metric: world X/Z/Y in metres (1 unit = 1 metre).
// This is gameplay-agnostic glue: it does NOT depend on engine/gameplay; the
// "farthest from enemies" XZ-distance pick is reimplemented here over plain
// Vec3 spans (cf. gameplay/MatchRules.cpp select_spawn).

#pragma once

#include "world/outdoor/Terrain.h"  // HeightmapDesc

#include "math/Math.h"
#include "core/Types.h"

#include <span>

namespace psynder::world::outdoor {

// Index of the candidate whose NEAREST enemy is farthest away, measuring only
// XZ distance (Y ignored). Ties break to the lowest index. Returns 0 if there
// are no candidates or no enemies. Deterministic, allocation-free.
usize select_farthest_spawn(std::span<const math::Vec3> candidates,
                            std::span<const math::Vec3> enemies) noexcept;

// Pick the farthest-from-enemies candidate (via select_farthest_spawn) and
// clamp it onto the terrain surface: Y = terrain_height(h, x, z) + foot_offset.
// Returns {0,0,0} if there are no candidates.
math::Vec3 pick_terrain_spawn(const HeightmapDesc& h,
                              std::span<const math::Vec3> candidates,
                              std::span<const math::Vec3> enemies,
                              f32 foot_offset = 0.0f) noexcept;

}  // namespace psynder::world::outdoor
