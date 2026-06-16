// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/ai/PathFollow.cpp — see PathFollow.h.

#include "ai/PathFollow.h"

#include "ai/PathSimplify.h"

#include <cmath>
#include <span>

namespace psynder::ai {

math::Vec3 cell_to_world(u32 cell_index, const GridLayout& layout) noexcept {
    // Decompose the index into grid coordinates and place the world point at the
    // cell's CENTRE: the +0.5 nudges off the min corner to the middle of the
    // [c, c+1) footprint. Pure +,-,*,/ — bit-identical across platforms.
    const u32 cx = (layout.width != 0u) ? (cell_index % layout.width) : 0u;
    const u32 cz = (layout.width != 0u) ? (cell_index / layout.width) : 0u;
    const f32 wx = layout.origin_x + (static_cast<f32>(cx) + 0.5f) * layout.cell_size_m;
    const f32 wz = layout.origin_z + (static_cast<f32>(cz) + 0.5f) * layout.cell_size_m;
    return math::Vec3{wx, 0.0f, wz};
}

bool plan_world_path(GridAStar& grid, u32 start_x, u32 start_z, u32 goal_x,
                     u32 goal_z, const GridLayout& layout,
                     std::vector<math::Vec3>& out_waypoints) {
    out_waypoints.clear();

    // Local scratch — planning is not the per-tick hot path, and these vectors
    // reuse their capacity across calls on a given thread, so steady-state
    // planning does not grow the heap once warm.
    static thread_local std::vector<u32> cell_path;
    static thread_local std::vector<u32> waypoint_cells;
    cell_path.clear();
    waypoint_cells.clear();

    if (!grid.find_path(start_x, start_z, goal_x, goal_z, cell_path))
        return false;  // no route: leave out_waypoints empty

    simplify_path(grid, std::span<const u32>(cell_path), waypoint_cells);

    out_waypoints.reserve(waypoint_cells.size());
    for (const u32 c : waypoint_cells)
        out_waypoints.push_back(cell_to_world(c, layout));
    return !out_waypoints.empty();
}

math::Vec3 follow_steer(PathFollower& f, math::Vec3 world_pos) noexcept {
    const usize n = f.waypoints.size();
    if (n == 0)            return math::Vec3{0.0f, 0.0f, 0.0f};  // no route
    if (f.current >= n)    return math::Vec3{0.0f, 0.0f, 0.0f};  // already done

    const f32 r2 = f.arrival_radius_m * f.arrival_radius_m;

    // Advance through any waypoints we already stand within arrival radius of
    // (XZ squared distance vs r^2 — no sqrt). Stopping at the last waypoint:
    // reaching it leaves current == n, which yields the zero vector below.
    while (f.current < n) {
        const math::Vec3 wp = f.waypoints[f.current];
        const f32 dx = wp.x - world_pos.x;
        const f32 dz = wp.z - world_pos.z;
        if ((dx * dx + dz * dz) <= r2) {
            ++f.current;  // reached this one; consider the next
            continue;
        }
        // Steer toward this still-distant waypoint: one guarded sqrt to make a
        // unit XZ vector (y == 0). The zero-length case is covered by the
        // arrival test above, but guard it anyway for determinism safety.
        const f32 len2 = dx * dx + dz * dz;
        if (len2 <= 0.0f) return math::Vec3{0.0f, 0.0f, 0.0f};
        const f32 inv = 1.0f / std::sqrt(len2);
        return math::Vec3{dx * inv, 0.0f, dz * inv};
    }

    return math::Vec3{0.0f, 0.0f, 0.0f};  // final waypoint reached
}

}  // namespace psynder::ai
