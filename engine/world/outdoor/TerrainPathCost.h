// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/world/outdoor/TerrainPathCost.h — slope-weighted terrain traversal
// cost for outdoor pathfinding. The outdoor analog to a grid A*'s uniform cost:
// flat ground is cheap, steeper ground costs more, and ground past the max
// walkable slope is impassable. Layered on TerrainSlope's `terrain_slope_updot`
// (= cos of the slope angle, 1 on flat ground) and `terrain_walkable`, so the
// cost field agrees exactly with the walkability gate the rest of the gameplay
// layer uses.
//
// Determinism: every value-path function here is pure ALGEBRA (+ - * /) plus a
// single sqrt for horizontal distance — NO acos / transcendentals — so it is
// IEEE-754 deterministic across arm64 / x86_64 / MSVC under the lane's strict-FP
// flags and SAFE on the lockstep tick. Same terrain + args => bit-identical
// costs cross-platform. Metric: world X/Z/Y in metres.

#pragma once

#include "world/outdoor/Terrain.h"  // HeightmapDesc

#include "core/Types.h"

namespace psynder::world::outdoor {

// Sentinel returned for ground steeper than the walkable gate: a finite, very
// large cost (NOT a true INFINITY, to keep arithmetic well-defined — averaging
// two endpoint costs, or summing edges in an A* open set, stays finite and
// ordered). Any real edge cost on walkable terrain is many orders of magnitude
// below this, so an A* will never expand through an impassable node. Callers
// should treat a cost >= kImpassableCost as "no edge".
inline constexpr f32 kImpassableCost = 1.0e30f;

// Slope-weighted MOVEMENT COST MULTIPLIER for the ground at world (wx, wz).
//
// `min_updot` is the walkability gate cos-threshold (e.g. cos(45deg) ~ 0.707):
// updot below it is too steep to traverse. `max_cost_mult` is the multiplier
// charged at the steepest still-walkable ground (should be >= 1).
//
// Returns:
//   - kImpassableCost            when updot <  min_updot (too steep).
//   - 1.0                        on flat ground (updot == 1).
//   - a value in (1, max_cost_mult] that rises linearly as updot approaches
//     min_updot:  1 + (max_cost_mult - 1) * (1 - updot) / (1 - min_updot),
//     clamped to [1, max_cost_mult].
//
// Pure algebra, lockstep-safe. (If min_updot >= 1 the slope band is degenerate;
// the function then returns 1.0 for any walkable ground.)
f32 terrain_move_cost(const HeightmapDesc& h, f32 wx, f32 wz, f32 min_updot,
                      f32 max_cost_mult) noexcept;

// Cheap walkability reject: true when the ground at world (wx, wz) is traversable
// (updot >= min_updot). Thin wrapper over `terrain_walkable`. Lockstep-safe.
bool terrain_passable(const HeightmapDesc& h, f32 wx, f32 wz,
                      f32 min_updot) noexcept;

// Cost to move from world A (ax, az) to world B (bx, bz):
//   horizontal_distance(A, B) * average(move_cost(A), move_cost(B))
// i.e. the straight-line horizontal length scaled by the mean per-node cost
// multiplier of the two endpoints. Returns kImpassableCost if EITHER endpoint
// is impassable (so an A* never relaxes an edge that touches non-walkable
// ground). Pure algebra plus one sqrt for the distance — lockstep-safe.
f32 terrain_edge_cost(const HeightmapDesc& h, f32 ax, f32 az, f32 bx, f32 bz,
                      f32 min_updot, f32 max_cost_mult) noexcept;

}  // namespace psynder::world::outdoor
