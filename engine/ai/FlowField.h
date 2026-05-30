// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/ai/FlowField.h
//
// A 2D (XZ-plane) grid flow field for DOTS mass-agent pathing toward a single
// goal — the classic "cost field from the goal + gradient" technique (Emerson,
// "Crowd Pathfinding and Steering Using Flow Field Tiles", and the RTS
// flow-field literature). build() runs a deterministic Dijkstra cost field from
// the goal over free cells (obstacle cells blocked, diagonals don't cut
// corners), then each free cell's flow points toward its lowest-cost 8-neighbour.
// sample() is O(1) per agent, so thousands of agents share one field and steer
// along it.
//
// Determinism (lockstep pillar): integer edge costs (10 cardinal / 14 diagonal),
// a min-heap keyed by (cost, cell-index) so ties are index-ordered, and
// fixed-order neighbour scans. Built -fno-fast-math; the only float is the final
// normalised direction. No per-tick heap alloc in sample().

#pragma once

#include "math/Math.h"

#include "core/Types.h"

#include <vector>

namespace psynder::ai {

inline constexpr u32 kUnreachable = 0xFFFFFFFFu;

class FlowField {
public:
    // Lay out a w x h grid of `cell_size` m cells; `origin` is the world XZ of
    // cell (0,0)'s min corner (origin.y is ignored). Clears any blocks.
    void resize(math::Vec3 origin, f32 cell_size, u32 width, u32 height);

    // Mark every cell whose footprint overlaps the XZ extent of `box` as blocked.
    void block_aabb(const math::Aabb& box);
    void clear_blocks();

    // Compute the cost field + flow directions for `goal_world`. Re-run when the
    // goal or blocks change. Cheap to call each tick for a moving goal at modest
    // grid sizes (Dijkstra over w*h cells).
    void build(math::Vec3 goal_world);

    // Unit XZ steering direction (y == 0) at a world position; {0,0,0} when the
    // cell is blocked, off-grid, the goal cell, or has no path to the goal.
    math::Vec3 sample(math::Vec3 world) const;

    u32  width() const noexcept { return w_; }
    u32  height() const noexcept { return h_; }
    bool blocked(u32 cx, u32 cz) const noexcept;
    u32  cost(u32 cx, u32 cz) const noexcept;  // kUnreachable if no path

private:
    bool to_cell(math::Vec3 world, u32& cx, u32& cz) const noexcept;

    math::Vec3 origin_{0.0f, 0.0f, 0.0f};
    f32        cell_ = 1.0f;
    u32        w_ = 0;
    u32        h_ = 0;
    std::vector<u8>         blocked_;  // w*h, 1 = obstacle
    std::vector<u32>        cost_;     // w*h, kUnreachable until built
    std::vector<math::Vec3> flow_;     // w*h, unit XZ dir toward the goal
};

}  // namespace psynder::ai
