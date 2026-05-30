// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/ai/PathFollow.h
//
// The reusable "A* -> string-pull -> steer along waypoints" follower — the
// per-agent path-execution layer that sits on top of GridAStar + simplify_path.
// GridAStar solves a start->goal cell path, simplify_path collapses it to a
// minimal set of corner waypoints, and PathFollow converts those waypoints into
// world XZ positions and steers an agent from one to the next. Where FlowField
// serves a shared goal sampled by thousands of agents, PathFollow gives one
// agent its own world-space route to walk.
//
// XZ-plane convention (matching FlowField): the grid lives in the world XZ
// plane with y == 0; cell (cx,cz) maps to a world centre, and steering
// directions are returned as UNIT XZ vectors with y == 0.
//
// Determinism (lockstep pillar): pure +,-,*,/ and at most one guarded sqrt
// (for the normalize). No trig / acos / RNG / platform branches — same inputs
// => bit-identical outputs on every run/platform. Arrival tests use squared
// distance vs arrival_radius^2 to avoid an extra sqrt. Built -fno-fast-math
// -ffp-contract=off. Planning reuses local scratch so steady-state calls do not
// grow the heap once the output vectors are warm.

#pragma once

#include "ai/GridAStar.h"

#include "math/Math.h"

#include "core/Types.h"

#include <vector>

namespace psynder::ai {

// Describes where a GridAStar grid sits in the world XZ plane. `cell_size_m` is
// the edge length of a cell in metres; (origin_x, origin_z) is the world XZ of
// cell (0,0)'s MIN corner; `width` is the grid width (used to decompose a cell
// index z*width + x). y is always 0 (the grid is flat on the XZ plane).
struct GridLayout {
    f32 cell_size_m = 1.0f;
    f32 origin_x = 0.0f;
    f32 origin_z = 0.0f;
    u32 width = 0;
};

// World-space CENTRE of `cell_index` (z*width + x) under `layout`:
//   x = origin_x + (cx + 0.5) * cell_size_m
//   z = origin_z + (cz + 0.5) * cell_size_m
//   y = 0
// where cx = cell_index % width, cz = cell_index / width. Pure algebra.
math::Vec3 cell_to_world(u32 cell_index, const GridLayout& layout) noexcept;

// Plan a world-space route: GridAStar.find_path -> simplify_path -> convert each
// surviving waypoint cell to its world centre via cell_to_world. `out_waypoints`
// is cleared then filled. Returns false (and leaves out_waypoints empty) when no
// path exists. Deterministic for identical inputs.
bool plan_world_path(GridAStar& grid, u32 start_x, u32 start_z, u32 goal_x,
                     u32 goal_z, const GridLayout& layout,
                     std::vector<math::Vec3>& out_waypoints);

// Per-agent path-execution state. `waypoints` is a route (typically from
// plan_world_path); `current` is the index of the waypoint being steered toward;
// `arrival_radius_m` is the XZ distance at which a waypoint counts as reached.
struct PathFollower {
    std::vector<math::Vec3> waypoints;
    usize current = 0;
    f32   arrival_radius_m = 0.5f;
};

// Unit XZ steering direction (y == 0) from `world_pos` toward the current
// waypoint. When `world_pos` is within `arrival_radius_m` (XZ distance) of the
// current waypoint, advances `current` to the next. Returns a ZERO vector
// {0,0,0} once the final waypoint is reached (current past the end, or within
// arrival radius of the last waypoint) and for an empty `waypoints` list.
// Deterministic, pure algebra (one guarded sqrt for the normalize).
math::Vec3 follow_steer(PathFollower& f, math::Vec3 world_pos) noexcept;

}  // namespace psynder::ai
