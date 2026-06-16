// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/ai_path_simplify.cpp — deterministic line-of-sight string pulling:
// a clear grid line is visible, a blocked cell breaks it, and an A* cell path
// collapses to its corner waypoints (bit-reproducibly).

#include "ai/PathSimplify.h"

#include "ai/GridAStar.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <span>
#include <vector>

using namespace psynder;
using namespace psynder::ai;

namespace {
bool is_member(const std::vector<u32>& path, u32 cell) {
    return std::find(path.begin(), path.end(), cell) != path.end();
}
}  // namespace

TEST_CASE("path-simplify: line of sight is clear across open ground", "[ai]") {
    GridAStar grid;
    grid.resize(10, 10);
    REQUIRE(line_of_sight(grid, 0, 0, 9, 0));   // straight row
    REQUIRE(line_of_sight(grid, 0, 0, 9, 9));   // diagonal
    REQUIRE(line_of_sight(grid, 2, 7, 8, 1));   // arbitrary slope
    REQUIRE(line_of_sight(grid, 4, 4, 4, 4));   // degenerate (same cell)
}

TEST_CASE("path-simplify: a blocking cell on the segment breaks line of sight",
          "[ai]") {
    GridAStar grid;
    grid.resize(10, 10);
    grid.set_blocked(5, 0, true);
    REQUIRE_FALSE(line_of_sight(grid, 0, 0, 9, 0));  // wall sits on the row
    // A parallel row above the block is still clear.
    REQUIRE(line_of_sight(grid, 0, 1, 9, 1));
}

TEST_CASE("path-simplify: thick diagonal does not cut a blocked corner", "[ai]") {
    GridAStar grid;
    grid.resize(8, 8);
    // Block the two cells flanking the (1,1) diagonal corner from (0,0)->(2,2):
    // the line must refuse to slip between them.
    grid.set_blocked(1, 0, true);
    grid.set_blocked(0, 1, true);
    REQUIRE_FALSE(line_of_sight(grid, 0, 0, 2, 2));
}

TEST_CASE("path-simplify: a straight open path collapses to two waypoints",
          "[ai]") {
    GridAStar grid;
    grid.resize(12, 12);
    std::vector<u32> cells;
    REQUIRE(grid.find_path(0, 5, 11, 5, cells));  // straight along row z=5
    REQUIRE(cells.size() >= 2u);

    std::vector<u32> wp;
    simplify_path(grid, std::span<const u32>(cells), wp);
    REQUIRE(wp.size() == 2u);
    REQUIRE(wp.front() == cells.front());
    REQUIRE(wp.back() == cells.back());
}

TEST_CASE("path-simplify: empty and single-cell inputs", "[ai]") {
    GridAStar grid;
    grid.resize(4, 4);

    std::vector<u32> wp{42, 43};  // pre-dirty to prove it's cleared
    simplify_path(grid, std::span<const u32>(), wp);
    REQUIRE(wp.empty());

    const u32 one[] = {1u * 4u + 2u};  // a single cell index
    simplify_path(grid, std::span<const u32>(one, 1), wp);
    REQUIRE(wp.size() == 1u);
    REQUIRE(wp.front() == one[0]);
}

TEST_CASE("path-simplify: an L-shaped detour keeps the corner and shrinks",
          "[ai]") {
    GridAStar grid;
    grid.resize(16, 16);
    // A wall on column x=8 spanning z in [0,12] forces the path up and over the
    // top of the wall (gap at z=13..15), producing an L-shaped staircase.
    for (u32 z = 0; z <= 12; ++z) grid.set_blocked(8, z, true);

    std::vector<u32> cells;
    REQUIRE(grid.find_path(0, 0, 15, 0, cells));
    REQUIRE(cells.size() > 4u);  // the detour is a long staircase

    std::vector<u32> wp;
    simplify_path(grid, std::span<const u32>(cells), wp);

    // First and last preserved.
    REQUIRE(wp.front() == cells.front());
    REQUIRE(wp.back() == cells.back());
    // Far fewer waypoints than raw cells, and at least one interior corner.
    REQUIRE(wp.size() < cells.size());
    REQUIRE(wp.size() >= 3u);
    REQUIRE(wp.size() * 2u <= cells.size());  // "much smaller"
    // Every waypoint is a member of the original path.
    for (const u32 c : wp) REQUIRE(is_member(cells, c));
    // Consecutive waypoints have clear line of sight (the string-pull invariant).
    const u32 w = grid.width();
    for (usize i = 1; i < wp.size(); ++i) {
        REQUIRE(line_of_sight(grid, wp[i - 1] % w, wp[i - 1] / w, wp[i] % w,
                              wp[i] / w));
    }
}

TEST_CASE("path-simplify: simplification is deterministic",
          "[ai][determinism]") {
    GridAStar grid;
    grid.resize(20, 20);
    for (u32 z = 2; z <= 16; ++z) grid.set_blocked(7, z, true);
    for (u32 z = 4; z <= 18; ++z) grid.set_blocked(13, z, true);

    std::vector<u32> cells;
    REQUIRE(grid.find_path(0, 0, 19, 19, cells));

    std::vector<u32> a;
    std::vector<u32> b;
    simplify_path(grid, std::span<const u32>(cells), a);
    simplify_path(grid, std::span<const u32>(cells), b);
    REQUIRE(a == b);
    REQUIRE(a.front() == cells.front());
    REQUIRE(a.back() == cells.back());
}
