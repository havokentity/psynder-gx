// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/ai_astar.cpp — deterministic grid A* point-to-point pathing: a
// straight line on open ground, routing through a wall's gap, an unreachable
// walled-off goal, and a bit-reproducible path.

#include "ai/GridAStar.h"

#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace psynder;
using namespace psynder::ai;

namespace {
// Cell index helper matching the grid layout (z*width + x).
u32 idx(u32 x, u32 z, u32 w) { return z * w + x; }
}  // namespace

TEST_CASE("astar: straight-line path on open ground", "[ai][astar]") {
    GridAStar a;
    a.resize(10, 10);

    std::vector<u32> path;
    REQUIRE(a.find_path(0, 5, 9, 5, path));
    // Endpoints are inclusive: start first, goal last.
    REQUIRE(path.front() == idx(0, 5, 10));
    REQUIRE(path.back() == idx(9, 5, 10));
    // A horizontal run of 9 steps => 10 cells.
    REQUIRE(path.size() == 10u);
    // Every cell on a pure +X line stays on row z=5.
    for (u32 c : path) REQUIRE(c / 10 == 5u);
}

TEST_CASE("astar: a wall with a gap routes through the gap", "[ai][astar]") {
    GridAStar a;
    a.resize(11, 11);
    // Block the whole x=5 column EXCEPT a single gap at z=10.
    for (u32 z = 0; z < 10; ++z) a.set_blocked(5, z, true);
    // z=10 left free => the only passage from left to right.

    std::vector<u32> path;
    REQUIRE(a.find_path(0, 5, 10, 5, path));
    REQUIRE(path.front() == idx(0, 5, 11));
    REQUIRE(path.back() == idx(10, 5, 11));

    // The path must never step on a blocked cell, and it must cross the wall
    // column through the gap row z=10.
    bool used_gap = false;
    for (u32 c : path) {
        const u32 x = c % 11;
        const u32 z = c / 11;
        REQUIRE_FALSE(a.blocked(x, z));
        if (x == 5) {
            REQUIRE(z == 10u);  // the only free cell in the wall column
            used_gap = true;
        }
    }
    REQUIRE(used_gap);
}

TEST_CASE("astar: a fully walled-off goal is unreachable", "[ai][astar]") {
    GridAStar a;
    a.resize(11, 11);
    // Seal cell (5,5) on all four cardinal sides; diagonals can't cut the
    // corners, so the pocket is fully isolated.
    a.set_blocked(4, 5, true);
    a.set_blocked(6, 5, true);
    a.set_blocked(5, 4, true);
    a.set_blocked(5, 6, true);

    std::vector<u32> path;
    REQUIRE_FALSE(a.find_path(0, 0, 5, 5, path));
    REQUIRE(path.empty());
}

TEST_CASE("astar: pathfinding is deterministic", "[ai][astar][determinism]") {
    const auto run = []() {
        GridAStar a;
        a.resize(16, 16);
        // A couple of partial walls to force branching with tie choices.
        for (u32 z = 2; z < 14; ++z) a.set_blocked(6, z, true);
        for (u32 z = 0; z < 12; ++z) a.set_blocked(10, z, true);
        std::vector<u32> path;
        a.find_path(0, 0, 15, 15, path);
        return path;
    };
    const std::vector<u32> first = run();
    const std::vector<u32> second = run();
    REQUIRE(first == second);
    REQUIRE_FALSE(first.empty());  // a route exists around the walls
}
