// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/ai/PathSimplify.cpp — see PathSimplify.h. Pure-integer grid line walk
// (supercover Bresenham with the thick-diagonal corner rule) + greedy string
// pull over a GridAStar cell path. No floats; deterministic for lockstep.

#include "ai/PathSimplify.h"

namespace psynder::ai {

bool line_of_sight(const GridAStar& grid, u32 ax, u32 az, u32 bx, u32 bz) noexcept {
    // Endpoints off-grid or themselves blocked => no clear line.
    if (ax >= grid.width() || az >= grid.height() || bx >= grid.width() ||
        bz >= grid.height()) {
        return false;
    }

    // Work in signed cell coordinates; step one cell at a time along the
    // dominant axis using an integer error term (Bresenham). On a diagonal step
    // we additionally require both orthogonally-adjacent cells to be clear so the
    // line can't squeeze through a blocked corner.
    i32 x = static_cast<i32>(ax);
    i32 z = static_cast<i32>(az);
    const i32 tx = static_cast<i32>(bx);
    const i32 tz = static_cast<i32>(bz);

    const i32 dx = (tx > x) ? (tx - x) : (x - tx);
    const i32 dz = (tz > z) ? (tz - z) : (z - tz);
    const i32 sx = (tx > x) ? 1 : -1;
    const i32 sz = (tz > z) ? 1 : -1;

    auto cell_blocked = [&grid](i32 cx, i32 cz) noexcept -> bool {
        return grid.blocked(static_cast<u32>(cx), static_cast<u32>(cz));
    };

    // Starting cell.
    if (cell_blocked(x, z)) return false;

    // err = dx - dz; positive favours an x-step, negative a z-step, zero a
    // diagonal. This integer DDA visits every cell the ideal line crosses.
    i32 err = dx - dz;
    while (x != tx || z != tz) {
        const i32 e2 = err * 2;
        const bool step_x = e2 > -dz;
        const bool step_z = e2 < dx;

        if (step_x && step_z) {
            // Diagonal move: both orthogonal neighbours of the corner must be
            // clear (no slipping between two blocked cells).
            if (cell_blocked(x + sx, z) || cell_blocked(x, z + sz)) return false;
            err += dx;
            err -= dz;
            x += sx;
            z += sz;
        } else if (step_x) {
            err -= dz;
            x += sx;
        } else {  // step_z
            err += dx;
            z += sz;
        }

        if (cell_blocked(x, z)) return false;
    }
    return true;
}

void simplify_path(const GridAStar& grid, std::span<const u32> cell_path,
                   std::vector<u32>& out_waypoints) {
    out_waypoints.clear();
    if (cell_path.empty()) return;

    const u32 w = grid.width();
    if (w == 0) {
        // Degenerate grid: preserve endpoints verbatim, can't reason about LOS.
        out_waypoints.push_back(cell_path.front());
        if (cell_path.size() > 1) out_waypoints.push_back(cell_path.back());
        return;
    }

    auto cell_x = [w](u32 cell) noexcept -> u32 { return cell % w; };
    auto cell_z = [w](u32 cell) noexcept -> u32 { return cell / w; };

    const usize n = cell_path.size();
    out_waypoints.push_back(cell_path[0]);
    if (n == 1) return;

    usize anchor = 0;  // index in cell_path of the current waypoint
    while (anchor + 1 < n) {
        const u32 ax = cell_x(cell_path[anchor]);
        const u32 az = cell_z(cell_path[anchor]);

        // Advance `farthest` as far as the straight line from the anchor stays
        // clear. It is always at least anchor+1 (consecutive path cells are
        // adjacent, so they are trivially visible).
        usize farthest = anchor + 1;
        for (usize j = anchor + 2; j < n; ++j) {
            if (line_of_sight(grid, ax, az, cell_x(cell_path[j]),
                              cell_z(cell_path[j]))) {
                farthest = j;
            } else {
                break;
            }
        }

        out_waypoints.push_back(cell_path[farthest]);
        anchor = farthest;
    }
}

}  // namespace psynder::ai
