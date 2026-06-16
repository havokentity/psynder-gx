// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/ai_path_follow.cpp — the per-agent "A* -> string-pull -> steer"
// follower: cell->world centre math, planning a world route on open and walled
// grids, an unreachable goal, steering that advances through waypoints and stops
// at the end, and bit-reproducible plan+follow output.

#include "ai/PathFollow.h"

#include "ai/GridAStar.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

using namespace psynder;
using namespace psynder::ai;

namespace {
// XZ distance between two world points (y ignored), for assertions.
f32 xz_dist(math::Vec3 a, math::Vec3 b) {
    const f32 dx = a.x - b.x;
    const f32 dz = a.z - b.z;
    return std::sqrt(dx * dx + dz * dz);
}
}  // namespace

TEST_CASE("path-follow: cell to world maps to the cell centre", "[ai]") {
    // 2 m cells, grid origin at world (10, *, -4), 8 cells wide.
    const GridLayout layout{2.0f, 10.0f, -4.0f, 8u};

    // Cell (0,0): centre at origin + half a cell.
    const math::Vec3 c00 = cell_to_world(0u, layout);
    REQUIRE(c00.x == Catch::Approx(10.0f + 1.0f));
    REQUIRE(c00.y == Catch::Approx(0.0f));
    REQUIRE(c00.z == Catch::Approx(-4.0f + 1.0f));

    // Cell (3,2) => index = z*width + x = 2*8 + 3 = 19.
    const math::Vec3 c = cell_to_world(2u * 8u + 3u, layout);
    REQUIRE(c.x == Catch::Approx(10.0f + (3.0f + 0.5f) * 2.0f));  // 17
    REQUIRE(c.y == Catch::Approx(0.0f));
    REQUIRE(c.z == Catch::Approx(-4.0f + (2.0f + 0.5f) * 2.0f));  // 1
}

TEST_CASE("path-follow: plan on open ground yields a start-to-goal route",
          "[ai]") {
    GridAStar grid;
    grid.resize(12, 12);
    const GridLayout layout{1.0f, 0.0f, 0.0f, 12u};

    std::vector<math::Vec3> wp{{99.0f, 99.0f, 99.0f}};  // pre-dirty
    REQUIRE(plan_world_path(grid, 1, 1, 10, 1, layout, wp));
    REQUIRE(wp.size() >= 2u);

    // First waypoint near the start cell centre, last near the goal cell centre.
    const math::Vec3 start_c = cell_to_world(1u * 12u + 1u, layout);
    const math::Vec3 goal_c = cell_to_world(1u * 12u + 10u, layout);
    REQUIRE(xz_dist(wp.front(), start_c) == Catch::Approx(0.0f));
    REQUIRE(xz_dist(wp.back(), goal_c) == Catch::Approx(0.0f));
    // The route is flat on the XZ plane.
    for (const math::Vec3 p : wp) REQUIRE(p.y == Catch::Approx(0.0f));
}

TEST_CASE("path-follow: a wall with a gap forces a non-straight route", "[ai]") {
    GridAStar grid;
    grid.resize(16, 16);
    // Wall on column x=8 spanning z in [0,12]; gap at z=13..15 forces a detour.
    for (u32 z = 0; z <= 12; ++z) grid.set_blocked(8, z, true);
    const GridLayout layout{1.0f, 0.0f, 0.0f, 16u};

    std::vector<math::Vec3> wp;
    REQUIRE(plan_world_path(grid, 0, 0, 15, 0, layout, wp));
    // A straight shot would be exactly two waypoints; the detour needs a corner.
    REQUIRE(wp.size() > 2u);
    // Still start- and goal-anchored.
    const math::Vec3 start_c = cell_to_world(0u, layout);
    const math::Vec3 goal_c = cell_to_world(0u * 16u + 15u, layout);
    REQUIRE(xz_dist(wp.front(), start_c) == Catch::Approx(0.0f));
    REQUIRE(xz_dist(wp.back(), goal_c) == Catch::Approx(0.0f));
}

TEST_CASE("path-follow: a walled-off goal makes planning fail", "[ai]") {
    GridAStar grid;
    grid.resize(11, 11);
    // Seal cell (5,5) on all four cardinal sides (diagonals can't cut corners).
    grid.set_blocked(4, 5, true);
    grid.set_blocked(6, 5, true);
    grid.set_blocked(5, 4, true);
    grid.set_blocked(5, 6, true);
    const GridLayout layout{1.0f, 0.0f, 0.0f, 11u};

    std::vector<math::Vec3> wp{{1.0f, 2.0f, 3.0f}};  // pre-dirty
    REQUIRE_FALSE(plan_world_path(grid, 0, 0, 5, 5, layout, wp));
    REQUIRE(wp.empty());  // false leaves the output empty
}

TEST_CASE("path-follow: steering points at the waypoint, advances, then stops",
          "[ai]") {
    PathFollower f;
    f.arrival_radius_m = 0.5f;
    // Two waypoints: +X then +Z from the origin.
    f.waypoints = {math::Vec3{10.0f, 0.0f, 0.0f}, math::Vec3{10.0f, 0.0f, 10.0f}};

    // From the origin, steer toward the first waypoint: pure +X, unit length.
    math::Vec3 d = follow_steer(f, math::Vec3{0.0f, 0.0f, 0.0f});
    REQUIRE(f.current == 0u);
    REQUIRE(d.x == Catch::Approx(1.0f));
    REQUIRE(d.y == Catch::Approx(0.0f));
    REQUIRE(d.z == Catch::Approx(0.0f));

    // Stand right on the first waypoint: it is consumed and we steer to #2 (+Z).
    d = follow_steer(f, math::Vec3{10.0f, 0.0f, 0.0f});
    REQUIRE(f.current == 1u);
    REQUIRE(d.x == Catch::Approx(0.0f));
    REQUIRE(d.z == Catch::Approx(1.0f));

    // Within arrival radius of the final waypoint: stop (zero vector).
    d = follow_steer(f, math::Vec3{10.0f, 0.0f, 9.8f});
    REQUIRE(f.current == 2u);  // advanced past the end
    REQUIRE(d.x == Catch::Approx(0.0f));
    REQUIRE(d.y == Catch::Approx(0.0f));
    REQUIRE(d.z == Catch::Approx(0.0f));

    // Subsequent calls keep returning zero (route complete).
    d = follow_steer(f, math::Vec3{0.0f, 0.0f, 0.0f});
    REQUIRE(d.x == Catch::Approx(0.0f));
    REQUIRE(d.z == Catch::Approx(0.0f));
}

TEST_CASE("path-follow: empty waypoints steer to zero", "[ai]") {
    PathFollower f;  // no waypoints
    const math::Vec3 d = follow_steer(f, math::Vec3{1.0f, 0.0f, 2.0f});
    REQUIRE(d.x == Catch::Approx(0.0f));
    REQUIRE(d.y == Catch::Approx(0.0f));
    REQUIRE(d.z == Catch::Approx(0.0f));
}

TEST_CASE("path-follow: plan and follow are deterministic",
          "[ai][determinism]") {
    const GridLayout layout{1.0f, 0.0f, 0.0f, 20u};

    const auto plan = []() {
        GridAStar grid;
        grid.resize(20, 20);
        // Two partial walls to force branching with tie choices.
        for (u32 z = 2; z <= 16; ++z) grid.set_blocked(7, z, true);
        for (u32 z = 4; z <= 18; ++z) grid.set_blocked(13, z, true);
        const GridLayout l{1.0f, 0.0f, 0.0f, 20u};
        std::vector<math::Vec3> wp;
        plan_world_path(grid, 0, 0, 19, 19, l, wp);
        return wp;
    };

    const std::vector<math::Vec3> a = plan();
    const std::vector<math::Vec3> b = plan();
    REQUIRE_FALSE(a.empty());
    REQUIRE(a.size() == b.size());
    // Bit-identical waypoint lists (component-wise exact equality).
    for (usize i = 0; i < a.size(); ++i) {
        REQUIRE(a[i].x == b[i].x);
        REQUIRE(a[i].y == b[i].y);
        REQUIRE(a[i].z == b[i].z);
    }

    // Walking both routes from the same positions yields identical steering.
    const math::Vec3 probes[] = {
        {0.0f, 0.0f, 0.0f}, {3.0f, 0.0f, 1.0f}, {9.0f, 0.0f, 9.0f},
        {15.0f, 0.0f, 12.0f}, {19.0f, 0.0f, 18.5f}};
    PathFollower fa{a, 0u, 0.5f};
    PathFollower fb{b, 0u, 0.5f};
    for (const math::Vec3 p : probes) {
        const math::Vec3 da = follow_steer(fa, p);
        const math::Vec3 db = follow_steer(fb, p);
        REQUIRE(da.x == db.x);
        REQUIRE(da.y == db.y);
        REQUIRE(da.z == db.z);
        REQUIRE(fa.current == fb.current);
    }
}
