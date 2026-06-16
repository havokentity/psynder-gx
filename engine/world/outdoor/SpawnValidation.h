// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/world/outdoor/SpawnValidation.h — deterministic walkable-spawn
// filtering on the heightfield for the Battlefield-light outdoor map. Filters
// candidate spawn points by terrain walkability (the lockstep-safe slope gate)
// and clamps the survivors onto the surface. This is the gameplay/match-side
// validation that COMPLEMENTS TerrainSpawn's "farthest from enemies" pick:
// TerrainSpawn chooses WHICH point, SpawnValidation decides WHICH points are
// legal to stand on in the first place.
//
// Determinism: every query here is pure algebra over the heightfield, layered
// on the lane's lockstep-safe primitives — `terrain_walkable` (the cos-threshold
// steepness gate, no transcendental) and `clamp_to_ground` (bilinear height +
// add). No acos, no RNG, no allocation beyond clearing/appending the caller's
// output vector. Same inputs => bitwise-identical outputs across arm64 /
// x86_64 / MSVC under the lane's strict-FP flags, so these are safe on the
// lockstep tick. Survivors are always emitted in ASCENDING candidate-index
// order — the ordering is part of the reproducibility contract. Metric: world
// X/Z/Y in metres (1 unit = 1 metre). `min_updot` is cos(max_walkable_angle),
// e.g. cos(45deg) ~ 0.707.

#pragma once

#include "world/outdoor/Terrain.h"  // HeightmapDesc

#include "math/Math.h"
#include "core/Types.h"

#include <span>
#include <vector>

namespace psynder::world::outdoor {

// Clear `out_indices`, then append (in ASCENDING candidate order) the index of
// every candidate whose ground is walkable, i.e. `terrain_walkable(h, c.x, c.z,
// min_updot)`. Only the candidate's XZ is sampled (its Y is irrelevant to the
// gate). Pure algebra over the heightfield — lockstep-safe.
void filter_walkable_spawns(const HeightmapDesc& h,
                            std::span<const math::Vec3> candidates,
                            f32 min_updot,
                            std::vector<usize>& out_indices) noexcept;

// Clear `out_points`, then for each WALKABLE candidate (same gate as
// filter_walkable_spawns) append `clamp_to_ground(h, candidate, foot_offset)` —
// the survivors snapped onto the terrain surface (Y = terrain_height + foot
// offset), in ASCENDING candidate order. The rejected (too-steep) candidates
// are dropped entirely. Lockstep-safe.
void clamp_walkable_spawns(const HeightmapDesc& h,
                           std::span<const math::Vec3> candidates,
                           f32 min_updot, f32 foot_offset,
                           std::vector<math::Vec3>& out_points) noexcept;

// True iff at least one candidate is walkable. Short-circuits on the first
// survivor (convenience predicate; cheaper than building the full index list).
bool any_walkable_spawn(const HeightmapDesc& h,
                        std::span<const math::Vec3> candidates,
                        f32 min_updot) noexcept;

// Lowest index of a walkable candidate, or `candidates.size()` if none is
// walkable. Short-circuits on the first survivor.
usize first_walkable_index(const HeightmapDesc& h,
                           std::span<const math::Vec3> candidates,
                           f32 min_updot) noexcept;

}  // namespace psynder::world::outdoor
