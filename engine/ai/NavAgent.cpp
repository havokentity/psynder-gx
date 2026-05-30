// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/ai/NavAgent.cpp — see NavAgent.h.

#include "ai/NavAgent.h"

#include <cmath>

namespace psynder::ai {

usize world_to_cell(math::Vec3 world_pos, const GridLayout& layout,
                    u32 grid_height) noexcept {
    // Degenerate grid: no valid cell, return index 0.
    if (layout.width == 0u || grid_height == 0u) return 0u;

    // Inverse of cell_to_world's centre math: subtract the origin, divide by the
    // cell edge, floor to a cell coordinate. floor handles negative offsets
    // (positions before the origin) so they clamp to the 0 edge rather than
    // wrapping. The division/floor are pure algebra — bit-identical everywhere.
    const f32 fx = std::floor((world_pos.x - layout.origin_x) / layout.cell_size_m);
    const f32 fz = std::floor((world_pos.z - layout.origin_z) / layout.cell_size_m);

    // Clamp into [0, width-1] x [0, height-1] so an off-grid agent maps to the
    // nearest edge cell (always a usable start cell for planning) instead of
    // being rejected. Compare as f32 against the bounds before the cast to keep
    // the clamp well-defined for large/negative inputs.
    const f32 max_x = static_cast<f32>(layout.width - 1u);
    const f32 max_z = static_cast<f32>(grid_height - 1u);
    const f32 cx = (fx < 0.0f) ? 0.0f : (fx > max_x ? max_x : fx);
    const f32 cz = (fz < 0.0f) ? 0.0f : (fz > max_z ? max_z : fz);

    return static_cast<usize>(cz) * static_cast<usize>(layout.width) +
           static_cast<usize>(cx);
}

void set_goal(NavAgent& a, u32 goal_x, u32 goal_z) noexcept {
    a.goal_x = goal_x;
    a.goal_z = goal_z;
    a.has_goal = true;
    a.has_path = false;  // force a replan on the next update()
}

bool replan(NavAgent& a, GridAStar& grid, math::Vec3 world_pos) {
    // Map the agent's world position to a start cell, decompose into (sx, sz),
    // then plan the world route into the follower's waypoint buffer.
    const usize start = world_to_cell(world_pos, a.layout, grid.height());
    const u32 sx = (a.layout.width != 0u)
                       ? static_cast<u32>(start % a.layout.width)
                       : 0u;
    const u32 sz = (a.layout.width != 0u)
                       ? static_cast<u32>(start / a.layout.width)
                       : 0u;

    a.has_path = plan_world_path(grid, sx, sz, a.goal_x, a.goal_z, a.layout,
                                 a.follower.waypoints);
    a.follower.current = 0u;  // walk the new route from its first waypoint
    return a.has_path;
}

math::Vec3 update(NavAgent& a, GridAStar& grid, math::Vec3 world_pos) {
    // No destination armed: nothing to steer toward.
    if (!a.has_goal) return math::Vec3{0.0f, 0.0f, 0.0f};

    // Lazily (re)plan when the goal changed or a prior plan failed/was cleared.
    if (!a.has_path) replan(a, grid, world_pos);

    // Still no path (unreachable goal): steer to zero.
    if (!a.has_path) return math::Vec3{0.0f, 0.0f, 0.0f};

    // Execute the route: follow_steer advances the cursor and returns the unit
    // XZ steer (or zero once the final waypoint is reached).
    return follow_steer(a.follower, world_pos);
}

}  // namespace psynder::ai
