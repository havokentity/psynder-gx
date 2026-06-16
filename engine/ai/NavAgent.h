// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/ai/NavAgent.h
//
// The per-agent "navigate to a world cell" controller — the thin glue a
// CombatBot embeds to turn a destination into a steering direction each tick.
// It owns a goal cell + a PathFollower and (re)plans on demand against a shared
// GridAStar, exposing a single update() call that returns the unit XZ steer.
// Where FlowField serves one shared goal sampled by thousands of agents and
// PathFollow is the raw route-execution primitive, NavAgent is the stateful
// wrapper: "set a goal, then call update() every tick and walk the result."
//
// XZ-plane convention (matching PathFollow / FlowField): the grid lives in the
// world XZ plane with y == 0; steering directions come back as UNIT XZ vectors
// with y == 0 (or the zero vector when there is nothing to steer toward).
//
// Determinism (lockstep pillar): pure +,-,*,/ and the single guarded sqrt that
// already lives inside follow_steer's normalize — no trig / acos / RNG /
// platform branches, so identical inputs yield bit-identical steer vectors and
// waypoint lists on every run/platform. world_to_cell clamps (rather than
// rejects) an off-grid position to the nearest edge cell so the start cell is
// always valid. Built -fno-fast-math -ffp-contract=off. The only allocation is
// inside replan()'s path build; steady-state update() calls do not grow the
// heap once the route vector is warm.

#pragma once

#include "ai/GridAStar.h"
#include "ai/PathFollow.h"

#include "math/Math.h"

#include "core/Types.h"

namespace psynder::ai {

// Inverse of cell_to_world: map a world XZ position to a cell index (cz*width +
// cx) under `layout`, clamping into [0,width) x [0,grid_height) so an off-grid
// position resolves to the nearest edge cell rather than being rejected. With
//   cx = floor((world.x - origin_x) / cell_size_m)  (clamped to [0,width-1]),
//   cz = floor((world.z - origin_z) / cell_size_m)  (clamped to [0,height-1]),
// the centre produced by cell_to_world round-trips back to the same index. Pure
// algebra (one floor each axis); returns 0 for a degenerate (empty) grid.
usize world_to_cell(math::Vec3 world_pos, const GridLayout& layout,
                    u32 grid_height) noexcept;

// Per-agent navigation state. `layout` describes where the grid sits in the
// world; (goal_x, goal_z) is the destination cell (valid only when has_goal);
// `follower` holds the planned world route + cursor; has_path is true once a
// successful plan is live. POD-ish: copy/assign give an independent agent.
struct NavAgent {
    GridLayout   layout;
    u32          goal_x = 0;
    u32          goal_z = 0;
    bool         has_goal = false;
    PathFollower follower;
    bool         has_path = false;
};

// Set the destination cell, arm has_goal, and clear has_path so the next
// update() forces a fresh replan from the agent's current position. Cheap, no
// planning here. Pure state mutation.
void set_goal(NavAgent& a, u32 goal_x, u32 goal_z) noexcept;

// Replan now: map `world_pos` to a start cell (world_to_cell), run
// plan_world_path(start -> goal) into the follower's waypoints, reset the
// cursor to the first waypoint, and store/return whether a route was found. On
// failure the waypoint list is left empty and has_path becomes false (update()
// then steers to zero). Allocates only inside the path build.
bool replan(NavAgent& a, GridAStar& grid, math::Vec3 world_pos);

// Per-tick call: when a goal is armed but no live path exists, replan from
// `world_pos`; then return the unit XZ steer toward the current waypoint via
// follow_steer. Returns the zero vector {0,0,0} when there is no goal, no path,
// or the route has been completed. Deterministic; allocation-free except for
// the replan path build on the tick a (re)plan happens.
math::Vec3 update(NavAgent& a, GridAStar& grid, math::Vec3 world_pos);

}  // namespace psynder::ai
