// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/world/outdoor/TerrainPathCost.cpp — see TerrainPathCost.h. Built on
// TerrainSlope's lockstep-safe `terrain_slope_updot` / `terrain_walkable` so the
// traversal cost field agrees exactly with the walkability gate the gameplay
// layer collides and spawns against.

#include "world/outdoor/TerrainPathCost.h"

#include "world/outdoor/TerrainSlope.h"  // terrain_slope_updot, terrain_walkable

#include <cmath>  // std::sqrt — deterministic (IEEE-754 correctly-rounded)

namespace psynder::world::outdoor {

f32 terrain_move_cost(const HeightmapDesc& h, f32 wx, f32 wz, f32 min_updot,
                      f32 max_cost_mult) noexcept {
    const f32 updot = terrain_slope_updot(h, wx, wz);

    // Too steep to traverse: not part of the navigable graph.
    if (updot < min_updot) return kImpassableCost;

    // A multiplier below 1 would make steeper ground "cheaper" than flat ground;
    // floor it at 1 so the cost is monotone in steepness.
    if (max_cost_mult < 1.0f) max_cost_mult = 1.0f;

    // Degenerate slope band (min_updot at/above flat): nothing to interpolate
    // over, so any walkable ground is flat-priced.
    const f32 denom = 1.0f - min_updot;
    if (denom <= 0.0f) return 1.0f;

    // Linear ramp: 1.0 at updot == 1 (flat), max_cost_mult at updot == min_updot.
    // Pure algebra (no transcendental) -> bit-identical across platforms.
    const f32 t = (1.0f - updot) / denom;            // 0 flat .. 1 at the gate
    f32 cost = 1.0f + (max_cost_mult - 1.0f) * t;

    // Clamp away FP overshoot at the band edges so the result stays in range.
    if (cost < 1.0f) cost = 1.0f;
    if (cost > max_cost_mult) cost = max_cost_mult;
    return cost;
}

bool terrain_passable(const HeightmapDesc& h, f32 wx, f32 wz,
                      f32 min_updot) noexcept {
    return terrain_walkable(h, wx, wz, min_updot);
}

f32 terrain_edge_cost(const HeightmapDesc& h, f32 ax, f32 az, f32 bx, f32 bz,
                      f32 min_updot, f32 max_cost_mult) noexcept {
    const f32 ca = terrain_move_cost(h, ax, az, min_updot, max_cost_mult);
    const f32 cb = terrain_move_cost(h, bx, bz, min_updot, max_cost_mult);

    // An edge touching impassable ground is itself impassable.
    if (ca >= kImpassableCost || cb >= kImpassableCost) return kImpassableCost;

    // Horizontal (X/Z) straight-line distance in metres: one sqrt, deterministic.
    const f32 dx = bx - ax;
    const f32 dz = bz - az;
    const f32 dist = std::sqrt(dx * dx + dz * dz);

    // Length scaled by the mean per-node cost multiplier of the two endpoints.
    const f32 avg_cost = 0.5f * (ca + cb);
    return dist * avg_cost;
}

}  // namespace psynder::world::outdoor
