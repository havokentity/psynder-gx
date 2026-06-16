// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/ai/CoverPoints.h
//
// Tactical cover-point detection on a GridAStar nav grid — the free cells a bot
// can stand at to take cover behind a wall. A free cell offers cover when at
// least one of its four orthogonal (cardinal) neighbours is a blocked obstacle:
// the bot tucks against the wall and is shielded from the side the wall is on.
// This is the spatial primitive a combat behaviour samples to pick a defensive
// stance ("get to the nearest cover and face away from the wall").
//
// Cover rule (documented once, applied everywhere): cell (x,z) is a cover cell
// iff it is in-range AND free (not blocked) AND at least one of its four cardinal
// neighbours (x+1,z) (x-1,z) (x,z+1) (x,z-1) is a real blocked cell. Off-grid
// neighbours are treated as NOT cover by default — only genuine obstacle cells
// count — so the open border of an obstacle-free grid is never flagged as cover.
// (Note this differs from GridAStar::blocked(), which reports off-grid as blocked
// for pathing safety; for cover we deliberately ignore the grid edge.)
//
// Determinism (lockstep pillar): pure integer grid math — no floats in the core,
// no RNG, no trig. Scans run in ascending cell-index order (z-major then x) and
// all ties break to the lowest cell index, so the same grid yields bit-identical
// cover lists on every run/platform. cell_to_world (PathFollow.h) is the only
// float touchpoint and is confined to the optional world-space helper, not used
// here.

#pragma once

#include "ai/GridAStar.h"

#include "core/Types.h"

#include <vector>

namespace psynder::ai {

// Bit layout for cover_direction(): which cardinal sides of a cover cell are
// blocked (i.e. where the sheltering wall is). A bot faces AWAY from these bits.
//   bit0 (0x1) = +X  neighbour (x+1, z) is blocked
//   bit1 (0x2) = -X  neighbour (x-1, z) is blocked
//   bit2 (0x4) = +Z  neighbour (x,   z+1) is blocked
//   bit3 (0x8) = -Z  neighbour (x,   z-1) is blocked
inline constexpr u32 kCoverPlusX  = 0x1u;  // wall on +X side
inline constexpr u32 kCoverMinusX = 0x2u;  // wall on -X side
inline constexpr u32 kCoverPlusZ  = 0x4u;  // wall on +Z side
inline constexpr u32 kCoverMinusZ = 0x8u;  // wall on -Z side

// True iff (x,z) is in-range, free, and has at least one blocked cardinal
// neighbour (per the cover rule above). Off-grid neighbours do NOT count.
bool is_cover_cell(const GridAStar& grid, u32 x, u32 z) noexcept;

// Clear `out_cells`, then append the cell index (z*width + x) of every cover
// cell, scanning in ascending index order (z-major then x). Deterministic.
void find_cover_cells(const GridAStar& grid, std::vector<u32>& out_cells) noexcept;

// Bitmask (see kCover* above) of which cardinal sides of (x,z) are blocked.
// Returns 0 for a non-cover or out-of-range cell (a free cell with no blocked
// cardinal neighbour also returns 0, since it is not a cover cell).
u32 cover_direction(const GridAStar& grid, u32 x, u32 z) noexcept;

// Find the cover cell with the smallest squared grid distance
// (dx*dx + dz*dz, pure integer) to (from_x, from_z); ties break to the lowest
// cell index. On success writes the cell index to `out_cell` and returns true.
// When the grid has no cover cells, returns false and leaves `out_cell`
// untouched. Deterministic. (A linear scan over all cells is used; a spatial
// acceleration structure is a follow-up if the grid grows very large.)
bool nearest_cover_cell(const GridAStar& grid, u32 from_x, u32 from_z,
                        u32& out_cell) noexcept;

}  // namespace psynder::ai
