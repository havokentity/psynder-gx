// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/ai/CoverPoints.cpp — see CoverPoints.h for the cover rule + bit layout.
//
// All math here is pure integer: in-range checks, cardinal-neighbour blocked
// tests (off-grid neighbours treated as NOT blocked for cover), z-major ascending
// scans, and an integer squared-distance nearest search with lowest-index ties.
// No floats, no RNG, no trig — same grid => bit-identical results everywhere.

#include "ai/CoverPoints.h"

namespace psynder::ai {

namespace {

// True iff (x,z) is a real in-range blocked obstacle cell. Unlike
// GridAStar::blocked() (which reports off-grid as blocked), this returns false
// for off-grid coordinates so the open grid border never counts toward cover.
// Inputs are u32, so a "negative" neighbour (x-1 when x==0) wraps to a huge
// value and is caught by the >= width/height range check below.
bool blocked_in_range(const GridAStar& grid, u32 x, u32 z) noexcept {
    if (x >= grid.width() || z >= grid.height()) return false;
    return grid.blocked(x, z);
}

// Cover-side bitmask for a cell ASSUMED in-range and free. Each cardinal
// neighbour that is a real blocked cell sets its bit (see kCover* in the header).
// Returns 0 when no cardinal neighbour is blocked (i.e. not actually cover).
u32 cover_bits(const GridAStar& grid, u32 x, u32 z) noexcept {
    u32 bits = 0u;
    if (blocked_in_range(grid, x + 1u, z)) bits |= kCoverPlusX;   // +X
    if (blocked_in_range(grid, x - 1u, z)) bits |= kCoverMinusX;  // -X
    if (blocked_in_range(grid, x, z + 1u)) bits |= kCoverPlusZ;   // +Z
    if (blocked_in_range(grid, x, z - 1u)) bits |= kCoverMinusZ;  // -Z
    return bits;
}

}  // namespace

bool is_cover_cell(const GridAStar& grid, u32 x, u32 z) noexcept {
    // In-range and free, then at least one blocked cardinal neighbour.
    if (x >= grid.width() || z >= grid.height()) return false;
    if (grid.blocked(x, z)) return false;
    return cover_bits(grid, x, z) != 0u;
}

void find_cover_cells(const GridAStar& grid, std::vector<u32>& out_cells) noexcept {
    out_cells.clear();
    const u32 w = grid.width();
    const u32 h = grid.height();
    // z-major then x => ascending cell index (z*w + x). Deterministic order.
    for (u32 z = 0u; z < h; ++z) {
        for (u32 x = 0u; x < w; ++x) {
            if (grid.blocked(x, z)) continue;          // cover cells are free
            if (cover_bits(grid, x, z) == 0u) continue;  // needs a wall neighbour
            out_cells.push_back(z * w + x);
        }
    }
}

u32 cover_direction(const GridAStar& grid, u32 x, u32 z) noexcept {
    // 0 for out-of-range or blocked cells; cover_bits is 0 for a free cell with
    // no blocked cardinal neighbour (i.e. not a cover cell), matching the header.
    if (x >= grid.width() || z >= grid.height()) return 0u;
    if (grid.blocked(x, z)) return 0u;
    return cover_bits(grid, x, z);
}

bool nearest_cover_cell(const GridAStar& grid, u32 from_x, u32 from_z,
                        u32& out_cell) noexcept {
    const u32 w = grid.width();
    const u32 h = grid.height();

    bool found = false;
    u64 best_d2 = 0;   // valid only once `found`; integer squared distance
    u32 best_idx = 0;  // valid only once `found`; lowest-index tie-break

    // Ascending z-major then x scan: because we keep a candidate only when it is
    // STRICTLY closer, the first (lowest-index) cell at the minimum distance wins
    // the tie automatically.
    for (u32 z = 0u; z < h; ++z) {
        for (u32 x = 0u; x < w; ++x) {
            if (grid.blocked(x, z)) continue;
            if (cover_bits(grid, x, z) == 0u) continue;

            // Signed deltas computed in i64 then squared — pure integer, no
            // u32 underflow, no float. dx*dx + dz*dz never overflows u64 for
            // any realistic grid size.
            const i64 dx = static_cast<i64>(x) - static_cast<i64>(from_x);
            const i64 dz = static_cast<i64>(z) - static_cast<i64>(from_z);
            const u64 d2 = static_cast<u64>(dx * dx + dz * dz);

            if (!found || d2 < best_d2) {
                found = true;
                best_d2 = d2;
                best_idx = z * w + x;
            }
        }
    }

    if (!found) return false;  // leave out_cell untouched
    out_cell = best_idx;
    return true;
}

}  // namespace psynder::ai
