// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/ai/PathSimplify.h
//
// Deterministic line-of-sight "string pulling" for GridAStar cell paths. A* on
// a grid yields a staircase of cell indices that hugs the lattice; most of those
// cells are redundant once you can walk a straight line between corners. This
// pass greedily collapses the path into a minimal set of corner waypoints: from
// the current waypoint it advances as far along the path as a clear straight
// grid line allows, emits the last visible cell, and repeats. Agents then steer
// between far-apart waypoints instead of cell-by-cell, smoothing the route.
//
// Determinism (lockstep pillar): pure integer math. line_of_sight() walks a
// supercover/Bresenham grid line cell-by-cell with a "thick" diagonal rule
// (when stepping diagonally, both orthogonal-adjacent cells must be clear), so
// the line never slips through a blocked corner — matching GridAStar/FlowField's
// no-corner-cutting policy. No floats anywhere; same grid + path => bit-identical
// out_waypoints on every run/platform.

#pragma once

#include "ai/GridAStar.h"

#include "core/Types.h"

#include <span>
#include <vector>

namespace psynder::ai {

// True iff every cell the straight grid line from (ax,az) to (bx,bz) passes
// through is free (not blocked). Deterministic integer supercover walk with the
// thick-diagonal corner rule. Endpoints are included; an endpoint that is itself
// blocked makes the result false.
bool line_of_sight(const GridAStar& grid, u32 ax, u32 az, u32 bx, u32 bz) noexcept;

// Greedy string-pull. `cell_path` is a GridAStar path (cell indices z*width+x);
// `out_waypoints` is cleared then filled with a subset of `cell_path` — always
// including the first and last cell — such that consecutive waypoints have clear
// line_of_sight between them. Empty input => empty output; single-cell input =>
// that one cell. Deterministic for identical inputs.
void simplify_path(const GridAStar& grid, std::span<const u32> cell_path,
                   std::vector<u32>& out_waypoints);

}  // namespace psynder::ai
