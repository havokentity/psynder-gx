// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/ai/Patrol.cpp — see Patrol.h.

#include "ai/Patrol.h"

namespace psynder::ai {

usize patrol_target_cell(const PatrolRoute& r, const GridLayout& layout) noexcept {
    // Empty route has no target cell; report index 0 (degenerate but defined).
    if (r.points.empty()) return 0u;

    // current is always kept in-bounds by start_patrol / update_patrol, but guard
    // defensively against a caller that set it past the end before the first tick.
    const usize idx = (r.current < r.points.size()) ? r.current : 0u;
    const PatrolPoint& p = r.points[idx];

    // Same index decomposition cell_to_world expects: z * width + x.
    return static_cast<usize>(p.z) * static_cast<usize>(layout.width) +
           static_cast<usize>(p.x);
}

void start_patrol(NavAgent& a, PatrolRoute& r) noexcept {
    // Nothing to patrol: leave the agent untouched.
    if (r.points.empty()) return;

    // Rewind to the first stop and point the agent at it (set_goal clears the
    // agent's path so the next update() replans from the bot's current position).
    r.current = 0u;
    set_goal(a, r.points[0].x, r.points[0].z);
}

math::Vec3 update_patrol(NavAgent& a, PatrolRoute& r, GridAStar& grid,
                         math::Vec3 world_pos) {
    // Empty route: nothing to steer toward.
    if (r.points.empty()) return math::Vec3{0.0f, 0.0f, 0.0f};

    // Keep the cursor in-bounds (defends against a caller starting mid-loop).
    if (r.current >= r.points.size()) r.current = 0u;

    // World centre of the current patrol point (its cell index under the agent's
    // layout). cell_to_world expects a z*width + x index, matching PatrolPoint.
    const usize target_cell = patrol_target_cell(r, a.layout);
    const math::Vec3 target = cell_to_world(static_cast<u32>(target_cell), a.layout);

    // ARRIVAL TEST: squared XZ distance vs arrival_radius^2 (no extra sqrt, same
    // convention as PathFollow). When within the radius the bot has reached this
    // point and we advance the cursor.
    const f32 dx = world_pos.x - target.x;
    const f32 dz = world_pos.z - target.z;
    const f32 dist_sq = dx * dx + dz * dz;
    const f32 radius = a.follower.arrival_radius_m;
    if (dist_sq <= radius * radius) {
        // Pick the next point: wrap to 0 past the last when looping, otherwise
        // hold on the last point (the bot parks there). last == current means no
        // change, so the goal is left as-is and the agent simply steers to zero.
        const usize last = r.points.size() - 1u;
        usize next = r.current;
        if (r.current < last) {
            next = r.current + 1u;  // advance toward the end
        } else if (r.loop) {
            next = 0u;              // looped past the last point -> back to start
        }
        // else: !loop on the last point -> stay (next == current).

        // Only re-arm the goal when the destination point actually changed; a
        // redundant set_goal would needlessly force a replan every tick while the
        // bot sits on a held last point.
        if (next != r.current) {
            r.current = next;
            set_goal(a, r.points[r.current].x, r.points[r.current].z);
        }
    }

    // Defer to NavAgent for the actual plan/steer toward the current goal. Returns
    // the zero vector once the route to the point is complete (or held).
    return update(a, grid, world_pos);
}

}  // namespace psynder::ai
