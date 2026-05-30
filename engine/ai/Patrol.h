// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/ai/Patrol.h
//
// A multi-point patrol controller layered on NavAgent — the behaviour a guard
// or roaming bot embeds to walk a fixed loop of grid waypoints. Where NavAgent
// drives one agent to one goal cell and stops, Patrol owns an ordered list of
// waypoints and a cursor: it points the NavAgent at the current waypoint,
// detects when the bot has physically reached it, then advances the cursor to
// the next waypoint (wrapping to the start when looping, or holding on the last
// point when not), re-arming the NavAgent's goal so it replans toward the new
// destination. The heavy lifting (A* plan, string-pull, steer) stays in
// NavAgent/PathFollow; Patrol is the thin sequencer on top.
//
// XZ-plane convention (matching NavAgent / PathFollow): the grid lives in the
// world XZ plane with y == 0; the steer returned by update_patrol is the same
// UNIT XZ vector NavAgent produces (or the zero vector when arrived / no path /
// empty route).
//
// Determinism (lockstep pillar): pure +,-,*,/ — the arrival test compares a
// squared XZ distance against arrival_radius^2 (no extra sqrt), and cursor
// advancement is integer index arithmetic. No trig / acos / RNG / platform
// branches, so identical inputs yield bit-identical steer vectors AND an
// identical `current` progression on every run/platform. The only allocation is
// the one already inside NavAgent::replan()'s path build on a (re)plan tick.

#pragma once

#include "ai/NavAgent.h"
#include "ai/PathFollow.h"

#include "math/Math.h"

#include "core/Types.h"

#include <vector>

namespace psynder::ai {

// One stop on a patrol route: the grid cell coordinate (x, z) the bot should
// walk to. Decomposes into a cell index `z * layout.width + x`.
struct PatrolPoint {
    u32 x = 0;
    u32 z = 0;
};

// An ordered patrol route plus a cursor. `points` is the loop of stops walked in
// order; `current` indexes the point currently being driven to; `loop` chooses
// what happens after the last point — wrap back to 0 (true) or hold on the last
// point (false). POD-ish: copy/assign give an independent route.
struct PatrolRoute {
    std::vector<PatrolPoint> points;
    usize                    current = 0;
    bool                     loop = true;
};

// (Re)start the patrol: reset the cursor to the first point and arm the agent's
// goal at `points[0]` (forcing a NavAgent replan on the next update). No-op for
// an empty route. Pure state mutation.
void start_patrol(NavAgent& a, PatrolRoute& r) noexcept;

// Per-tick call. Drives `a` toward `r.points[r.current]`:
//   1. ARRIVAL TEST — if the XZ distance from `world_pos` to the current point's
//      world centre (cell_to_world of `current`'s cell index) is within
//      a.follower.arrival_radius_m, the bot has reached this point. Advance the
//      cursor: to the next index, wrapping to 0 when looping past the last point,
//      or staying put on the last point when not looping. When the target point
//      actually changes, call set_goal() for the new point (forcing a replan).
//   2. Return update(a, grid, world_pos) — the NavAgent steer toward the current
//      goal (the zero vector once arrived with no path, or for a held last point).
// Returns the zero vector {0,0,0} for an empty route. Deterministic; the only
// allocation is NavAgent::replan()'s path build on a (re)plan tick.
math::Vec3 update_patrol(NavAgent& a, PatrolRoute& r, GridAStar& grid,
                         math::Vec3 world_pos);

// Read helper: the cell index (z * layout.width + x) of the route's current
// point under `layout`, or 0 for an empty route. Pure algebra, no mutation.
usize patrol_target_cell(const PatrolRoute& r, const GridLayout& layout) noexcept;

}  // namespace psynder::ai
