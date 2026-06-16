// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/ai/GridAStar.h
//
// A 2D (XZ-plane) grid A* pathfinder for point-to-point agent paths — the
// per-agent complement to the shared-goal FlowField. Where the flow field bakes
// a single cost field every agent samples, GridAStar solves one explicit
// start->goal cell path on demand (e.g. for a hero/boss with its own
// destination, or a one-off route query).
//
// Determinism (lockstep pillar): same grid + endpoints => a bit-identical
// out_cells on every run/platform. We use integer g-costs (10 cardinal /
// 14 diagonal ~= 10*sqrt2), an admissible integer octile heuristic
// h = 14*min(dx,dz) + 10*|dx-dz|, and a binary min-heap keyed by (f_cost,
// cell_index) so ties break by lowest cell index — no floats in the search and
// no reliance on std::priority_queue's unspecified equal-key ordering. Diagonals
// never cut between two blocked orthogonal neighbours, matching FlowField.
// Internal scratch is sized once in resize() and reused across calls (no
// per-query heap alloc after the grid is laid out).

#pragma once

#include "core/Types.h"

#include <vector>

namespace psynder::ai {

class GridAStar {
public:
    // Lay out a width x height grid of free cells (clears any blocks) and size
    // the internal scratch buffers to width*height.
    void resize(u32 width, u32 height);

    // Mark cell (x, z) blocked (obstacle) or free. Out-of-range is ignored.
    void set_blocked(u32 x, u32 z, bool blocked);

    // Reset every cell to free (clears all blocks); grid dimensions are kept.
    void block_all_clear();

    // A* from (start_x, start_z) to (goal_x, goal_z). Returns true and fills
    // `out_cells` with the cell-index path (z*width + x) from start to goal
    // inclusive when a route exists; returns false and leaves `out_cells` empty
    // when the goal is unreachable, an endpoint is blocked, or an endpoint is
    // off-grid. Deterministic: identical inputs => identical out_cells.
    bool find_path(u32 start_x, u32 start_z, u32 goal_x, u32 goal_z,
                   std::vector<u32>& out_cells);

    u32  width() const noexcept { return w_; }
    u32  height() const noexcept { return h_; }
    bool blocked(u32 x, u32 z) const noexcept;

private:
    u32 w_ = 0;
    u32 h_ = 0;
    std::vector<u8>  blocked_;  // w*h, 1 = obstacle

    // Reused scratch (sized to w*h in resize()).
    std::vector<u32> g_;        // best known g-cost per cell (kInf until seen)
    std::vector<u32> parent_;   // predecessor cell index on the best path
    std::vector<u8>  closed_;   // 1 once a cell is finalised
    std::vector<u8>  open_;     // 1 while a cell sits in the open heap

    // Binary min-heap of cell indices, ordered by (f_cost, cell_index).
    std::vector<u32> heap_;     // cell indices
    std::vector<u32> f_;        // f-cost per cell (key for the heap order)
};

}  // namespace psynder::ai
