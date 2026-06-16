// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/ai_nav_agent.cpp — the per-agent "navigate to a world cell"
// controller layered on PathFollow + GridAStar: world_to_cell as the inverse of
// cell_to_world (round-trip + clamp), planning on the first update and steering
// toward the goal on open ground, a wall+gap forcing a non-straight route,
// stopping (zero steer) at the goal, set_goal forcing a replan, a walled-off
// goal making the plan fail, and bit-reproducible steer sequences.

#include "ai/NavAgent.h"

#include "ai/GridAStar.h"
#include "ai/PathFollow.h"

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

// Advance a world position one fixed step along a unit steer (XZ plane).
math::Vec3 step_along(math::Vec3 pos, math::Vec3 dir, f32 step_m) {
    return math::Vec3{pos.x + dir.x * step_m, 0.0f, pos.z + dir.z * step_m};
}
}  // namespace

TEST_CASE("nav-agent: world_to_cell inverts cell_to_world centres", "[ai]") {
    // 2 m cells, origin at world (10, *, -4), 8 wide x 6 tall.
    const GridLayout layout{2.0f, 10.0f, -4.0f, 8u};
    const u32 height = 6u;

    // Round-trip several cell centres back to their own index.
    const u32 cells[] = {0u, 3u, 8u + 1u, 2u * 8u + 5u, 5u * 8u + 7u};
    for (const u32 c : cells) {
        const math::Vec3 centre = cell_to_world(c, layout);
        REQUIRE(world_to_cell(centre, layout, height) == static_cast<usize>(c));
    }

    // Anywhere inside a cell's footprint resolves to that same cell.
    const math::Vec3 inside = cell_to_world(2u * 8u + 5u, layout);
    REQUIRE(world_to_cell(math::Vec3{inside.x + 0.9f, 0.0f, inside.z - 0.9f},
                          layout, height) == static_cast<usize>(2u * 8u + 5u));
}

TEST_CASE("nav-agent: world_to_cell clamps out-of-bounds positions", "[ai]") {
    const GridLayout layout{1.0f, 0.0f, 0.0f, 8u};
    const u32 height = 6u;

    // Far before the origin clamps to cell (0,0).
    REQUIRE(world_to_cell(math::Vec3{-100.0f, 0.0f, -100.0f}, layout, height) ==
            0u);
    // Far past the far corner clamps to the last cell (width-1, height-1).
    const usize last = static_cast<usize>(height - 1u) * 8u + (8u - 1u);
    REQUIRE(world_to_cell(math::Vec3{1000.0f, 0.0f, 1000.0f}, layout, height) ==
            last);
    // Off only one axis clamps just that axis.
    REQUIRE(world_to_cell(math::Vec3{1000.0f, 0.0f, 2.5f}, layout, height) ==
            static_cast<usize>(2u) * 8u + (8u - 1u));
}

TEST_CASE("nav-agent: plans on first update and steers toward the goal",
          "[ai]") {
    GridAStar grid;
    grid.resize(16, 16);
    const GridLayout layout{1.0f, 0.0f, 0.0f, 16u};

    NavAgent agent;
    agent.layout = layout;
    agent.follower.arrival_radius_m = 0.5f;
    set_goal(agent, 14u, 8u);
    REQUIRE(agent.has_goal);
    REQUIRE_FALSE(agent.has_path);  // nothing planned yet

    // Start near cell (2,8); first update should plan and steer roughly +X
    // (toward the goal, which lies in the +X direction).
    const math::Vec3 start = cell_to_world(8u * 16u + 2u, layout);
    const math::Vec3 dir = update(agent, grid, start);
    REQUIRE(agent.has_path);  // planned on the first tick
    REQUIRE_FALSE(agent.follower.waypoints.empty());

    // Steer is a unit XZ vector pointing toward the goal (positive dot with the
    // goal direction).
    const math::Vec3 goal = cell_to_world(8u * 16u + 14u, layout);
    const f32 gx = goal.x - start.x;
    const f32 gz = goal.z - start.z;
    REQUIRE((dir.x * dir.x + dir.z * dir.z) == Catch::Approx(1.0f));
    REQUIRE(dir.y == Catch::Approx(0.0f));
    REQUIRE((dir.x * gx + dir.z * gz) > 0.0f);
}

TEST_CASE("nav-agent: a wall with a gap forces a non-straight route", "[ai]") {
    GridAStar grid;
    grid.resize(16, 16);
    // Wall on column x=8 spanning z in [0,12]; the gap at z=13..15 forces a
    // detour around the top.
    for (u32 z = 0; z <= 12; ++z) grid.set_blocked(8, z, true);
    const GridLayout layout{1.0f, 0.0f, 0.0f, 16u};

    NavAgent agent;
    agent.layout = layout;
    agent.follower.arrival_radius_m = 0.5f;
    set_goal(agent, 15u, 0u);

    const math::Vec3 start = cell_to_world(0u, layout);  // cell (0,0)
    const math::Vec3 dir = update(agent, grid, start);
    REQUIRE(agent.has_path);

    // A straight shot would be exactly two waypoints; the detour needs a corner,
    // and the first steer cannot point straight at the goal cell.
    const math::Vec3 goal = cell_to_world(0u * 16u + 15u, layout);
    const f32 straight = xz_dist(start, goal);
    const bool detoured = agent.follower.waypoints.size() > 2u;
    // First steer is not the straight-line bearing (the wall is in the way).
    const f32 sx = (goal.x - start.x) / straight;
    const f32 sz = (goal.z - start.z) / straight;
    const bool not_straight = (dir.x != Catch::Approx(sx)) ||
                              (dir.z != Catch::Approx(sz));
    REQUIRE((detoured || not_straight));
}

TEST_CASE("nav-agent: reaching the goal yields a zero steer", "[ai]") {
    GridAStar grid;
    grid.resize(12, 12);
    const GridLayout layout{1.0f, 0.0f, 0.0f, 12u};

    NavAgent agent;
    agent.layout = layout;
    agent.follower.arrival_radius_m = 0.5f;
    set_goal(agent, 9u, 6u);

    // Walk the agent toward the goal in small steps; it must arrive and then
    // steer to zero (route complete).
    math::Vec3 pos = cell_to_world(6u * 12u + 1u, layout);  // cell (1,6)
    const math::Vec3 goal = cell_to_world(6u * 12u + 9u, layout);
    math::Vec3 dir{1.0f, 0.0f, 0.0f};
    bool arrived = false;
    for (int i = 0; i < 400; ++i) {
        dir = update(agent, grid, pos);
        if (dir.x == Catch::Approx(0.0f) && dir.z == Catch::Approx(0.0f)) {
            arrived = true;
            break;
        }
        pos = step_along(pos, dir, 0.1f);
    }
    REQUIRE(arrived);
    REQUIRE(xz_dist(pos, goal) < 1.0f);  // stopped at the goal

    // A subsequent update keeps returning zero (nothing left to walk).
    const math::Vec3 d2 = update(agent, grid, pos);
    REQUIRE(d2.x == Catch::Approx(0.0f));
    REQUIRE(d2.z == Catch::Approx(0.0f));
}

TEST_CASE("nav-agent: set_goal forces a replan to a new route", "[ai]") {
    GridAStar grid;
    grid.resize(16, 16);
    const GridLayout layout{1.0f, 0.0f, 0.0f, 16u};

    NavAgent agent;
    agent.layout = layout;
    agent.follower.arrival_radius_m = 0.5f;

    const math::Vec3 start = cell_to_world(8u * 16u + 1u, layout);  // (1,8)

    // First goal -> a route, anchored at the first goal cell.
    set_goal(agent, 14u, 8u);
    update(agent, grid, start);
    REQUIRE(agent.has_path);
    REQUIRE_FALSE(agent.follower.waypoints.empty());
    const math::Vec3 goal_a = cell_to_world(8u * 16u + 14u, layout);
    REQUIRE(xz_dist(agent.follower.waypoints.back(), goal_a) ==
            Catch::Approx(0.0f));

    // Re-goal clears the path and forces a fresh plan to the NEW cell.
    set_goal(agent, 2u, 1u);
    REQUIRE_FALSE(agent.has_path);  // set_goal cleared it
    update(agent, grid, start);
    REQUIRE(agent.has_path);
    REQUIRE_FALSE(agent.follower.waypoints.empty());
    // The cursor was reset to 0 by the replan; because `start` coincides with the
    // first waypoint (the start cell centre), the same update's follow_steer
    // legitimately consumes it. The robust invariant is that the agent is
    // actively walking a fresh, not-yet-exhausted route.
    REQUIRE(agent.follower.current < agent.follower.waypoints.size());
    const math::Vec3 goal_b = cell_to_world(1u * 16u + 2u, layout);
    REQUIRE(xz_dist(agent.follower.waypoints.back(), goal_b) ==
            Catch::Approx(0.0f));
}

TEST_CASE("nav-agent: a walled-off goal makes replan fail and update steer zero",
          "[ai]") {
    GridAStar grid;
    grid.resize(11, 11);
    // Seal cell (5,5) on all four cardinal sides (diagonals can't cut corners).
    grid.set_blocked(4, 5, true);
    grid.set_blocked(6, 5, true);
    grid.set_blocked(5, 4, true);
    grid.set_blocked(5, 6, true);
    const GridLayout layout{1.0f, 0.0f, 0.0f, 11u};

    NavAgent agent;
    agent.layout = layout;
    set_goal(agent, 5u, 5u);

    const math::Vec3 start = cell_to_world(0u, layout);  // (0,0)
    REQUIRE_FALSE(replan(agent, grid, start));
    REQUIRE_FALSE(agent.has_path);
    REQUIRE(agent.follower.waypoints.empty());

    // update() also steers to zero when the goal is unreachable.
    const math::Vec3 dir = update(agent, grid, start);
    REQUIRE(dir.x == Catch::Approx(0.0f));
    REQUIRE(dir.y == Catch::Approx(0.0f));
    REQUIRE(dir.z == Catch::Approx(0.0f));
}

TEST_CASE("nav-agent: identical agents produce bit-identical steer sequences",
          "[ai][determinism]") {
    const GridLayout layout{1.0f, 0.0f, 0.0f, 20u};

    const auto build_grid = []() {
        GridAStar g;
        g.resize(20, 20);
        // Two partial walls to force branching with tie choices.
        for (u32 z = 2; z <= 16; ++z) g.set_blocked(7, z, true);
        for (u32 z = 4; z <= 18; ++z) g.set_blocked(13, z, true);
        return g;
    };

    const auto make_agent = [&]() {
        NavAgent a;
        a.layout = layout;
        a.follower.arrival_radius_m = 0.5f;
        set_goal(a, 19u, 19u);
        return a;
    };

    GridAStar grid_a = build_grid();
    GridAStar grid_b = build_grid();
    NavAgent a = make_agent();
    NavAgent b = make_agent();

    // Walk both agents in lockstep, advancing each by its own returned steer so
    // waypoint advancement is exercised. Steer vectors must match bit-for-bit.
    math::Vec3 pa = cell_to_world(0u, layout);
    math::Vec3 pb = pa;
    for (int i = 0; i < 64; ++i) {
        const math::Vec3 da = update(a, grid_a, pa);
        const math::Vec3 db = update(b, grid_b, pb);
        REQUIRE(da.x == db.x);
        REQUIRE(da.y == db.y);
        REQUIRE(da.z == db.z);
        REQUIRE(a.follower.current == b.follower.current);
        pa = step_along(pa, da, 0.25f);
        pb = step_along(pb, db, 0.25f);
    }

    // Both routes were planned identically, too.
    REQUIRE(a.follower.waypoints.size() == b.follower.waypoints.size());
    for (usize k = 0; k < a.follower.waypoints.size(); ++k) {
        REQUIRE(a.follower.waypoints[k].x == b.follower.waypoints[k].x);
        REQUIRE(a.follower.waypoints[k].y == b.follower.waypoints[k].y);
        REQUIRE(a.follower.waypoints[k].z == b.follower.waypoints[k].z);
    }
}
