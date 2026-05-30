// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/ai_cover_points.cpp — deterministic tactical cover-point detection
// on the nav grid: an open grid has no cover (the border is not cover), a single
// obstacle makes exactly its four orthogonal free neighbours cover (diagonals
// not), cover_direction reports the wall side, a corner free cell with two
// blocked neighbours yields two set bits, nearest_cover_cell finds the closest
// with a lowest-index tie-break (and false on an obstacle-free grid), the cover
// list is ascending, and two identical grids yield identical lists.

#include "ai/CoverPoints.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace psynder;
using namespace psynder::ai;

namespace {
// Cell index helper matching the grid layout (z*width + x).
u32 idx(u32 x, u32 z, u32 w) { return z * w + x; }
}  // namespace

TEST_CASE("cover: an open grid has no cover cells", "[ai][cover]") {
    GridAStar a;
    a.resize(8, 8);  // no obstacles

    REQUIRE_FALSE(is_cover_cell(a, 0, 0));  // corner: off-grid edges don't count
    REQUIRE_FALSE(is_cover_cell(a, 0, 4));  // border: off-grid edge doesn't count
    REQUIRE_FALSE(is_cover_cell(a, 4, 4));  // interior

    std::vector<u32> cover;
    find_cover_cells(a, cover);
    REQUIRE(cover.empty());

    // No cover => nearest_cover_cell fails and leaves out_cell untouched.
    u32 out = 0xDEADBEEFu;
    REQUIRE_FALSE(nearest_cover_cell(a, 3, 3, out));
    REQUIRE(out == 0xDEADBEEFu);
}

TEST_CASE("cover: a single obstacle makes its four orthogonal neighbours cover",
          "[ai][cover]") {
    GridAStar a;
    a.resize(7, 7);
    a.set_blocked(3, 3, true);  // a lone wall cell in the middle

    // The four cardinal neighbours of the obstacle are cover cells.
    REQUIRE(is_cover_cell(a, 4, 3));  // +X of obstacle
    REQUIRE(is_cover_cell(a, 2, 3));  // -X of obstacle
    REQUIRE(is_cover_cell(a, 3, 4));  // +Z of obstacle
    REQUIRE(is_cover_cell(a, 3, 2));  // -Z of obstacle

    // The four DIAGONAL neighbours are NOT cover (no blocked cardinal neighbour).
    REQUIRE_FALSE(is_cover_cell(a, 4, 4));
    REQUIRE_FALSE(is_cover_cell(a, 2, 2));
    REQUIRE_FALSE(is_cover_cell(a, 4, 2));
    REQUIRE_FALSE(is_cover_cell(a, 2, 4));

    // The obstacle cell itself is blocked => never cover.
    REQUIRE_FALSE(is_cover_cell(a, 3, 3));

    // find_cover_cells returns EXACTLY the four cardinal neighbours, ascending.
    std::vector<u32> cover;
    find_cover_cells(a, cover);
    const std::vector<u32> expected = {
        idx(3, 2, 7),  // -Z, index 17
        idx(2, 3, 7),  // -X, index 23
        idx(4, 3, 7),  // +X, index 25
        idx(3, 4, 7),  // +Z, index 31
    };
    REQUIRE(cover == expected);
}

TEST_CASE("cover: cover_direction reports the side the wall is on",
          "[ai][cover]") {
    GridAStar a;
    a.resize(7, 7);
    a.set_blocked(3, 3, true);

    // The cell to the +X of the obstacle has the wall on its -X side.
    REQUIRE(cover_direction(a, 4, 3) == kCoverMinusX);
    // The cell to the -X of the obstacle has the wall on its +X side.
    REQUIRE(cover_direction(a, 2, 3) == kCoverPlusX);
    // The cell to the +Z of the obstacle has the wall on its -Z side.
    REQUIRE(cover_direction(a, 3, 4) == kCoverMinusZ);
    // The cell to the -Z of the obstacle has the wall on its +Z side.
    REQUIRE(cover_direction(a, 3, 2) == kCoverPlusZ);

    // Non-cover and out-of-range cells report 0.
    REQUIRE(cover_direction(a, 4, 4) == 0u);   // diagonal, not cover
    REQUIRE(cover_direction(a, 3, 3) == 0u);   // the blocked obstacle itself
    REQUIRE(cover_direction(a, 100, 100) == 0u);  // out of range
}

TEST_CASE("cover: a corner free cell with two blocked neighbours has two bits",
          "[ai][cover]") {
    GridAStar a;
    a.resize(7, 7);
    // An L of walls so that the free cell (3,3) has a blocked cell on its +X
    // and on its +Z side simultaneously.
    a.set_blocked(4, 3, true);  // +X of (3,3)
    a.set_blocked(3, 4, true);  // +Z of (3,3)

    REQUIRE(is_cover_cell(a, 3, 3));
    const u32 dir = cover_direction(a, 3, 3);
    REQUIRE((dir & kCoverPlusX) != 0u);
    REQUIRE((dir & kCoverPlusZ) != 0u);
    REQUIRE((dir & kCoverMinusX) == 0u);
    REQUIRE((dir & kCoverMinusZ) == 0u);
    REQUIRE(dir == (kCoverPlusX | kCoverPlusZ));  // exactly two bits set
}

TEST_CASE("cover: nearest_cover_cell finds the closest with lowest-index tie",
          "[ai][cover]") {
    GridAStar a;
    a.resize(11, 11);
    a.set_blocked(5, 5, true);  // cover at (6,5),(4,5),(5,6),(5,4)

    // A query right next to one cover cell picks that one (it is strictly
    // closest at squared distance 1).
    u32 out = 0u;
    REQUIRE(nearest_cover_cell(a, 7, 5, out));  // nearest cover is (6,5), d2=1
    REQUIRE(out == idx(6, 5, 11));

    // A query equidistant from several cover cells breaks the tie to the lowest
    // cell index. From the obstacle centre (5,5) all four neighbours are at
    // squared distance 1; the lowest index is (5,4) (-Z neighbour, index 49).
    u32 tie = 0u;
    REQUIRE(nearest_cover_cell(a, 5, 5, tie));
    REQUIRE(tie == idx(5, 4, 11));  // lowest-index of the four tied neighbours

    // An obstacle-free grid has no cover => false, out untouched.
    GridAStar open;
    open.resize(6, 6);
    u32 untouched = 0x1234u;
    REQUIRE_FALSE(nearest_cover_cell(open, 2, 2, untouched));
    REQUIRE(untouched == 0x1234u);
}

TEST_CASE("cover: find_cover_cells output is in ascending index order",
          "[ai][cover]") {
    GridAStar a;
    a.resize(9, 9);
    // A few scattered obstacles to produce many cover cells.
    a.set_blocked(2, 2, true);
    a.set_blocked(6, 3, true);
    a.set_blocked(4, 6, true);

    std::vector<u32> cover;
    find_cover_cells(a, cover);
    REQUIRE_FALSE(cover.empty());
    for (usize i = 1; i < cover.size(); ++i) {
        REQUIRE(cover[i - 1] < cover[i]);  // strictly ascending, no duplicates
    }
}

TEST_CASE("cover: detection is deterministic across identical grids",
          "[ai][cover][determinism]") {
    const auto build = []() {
        GridAStar a;
        a.resize(12, 12);
        a.set_blocked(3, 3, true);
        a.set_blocked(8, 5, true);
        a.set_blocked(5, 9, true);
        for (u32 z = 1; z < 11; ++z) a.set_blocked(6, z, true);  // a wall column
        std::vector<u32> cover;
        find_cover_cells(a, cover);
        return cover;
    };
    REQUIRE(build() == build());
}
