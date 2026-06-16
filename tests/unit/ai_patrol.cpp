// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/ai_patrol.cpp — the multi-point patrol controller layered on
// NavAgent: a route visited in order (the bot passes within arrival of each
// point in sequence), looping wraps back to point 0 after the last, a non-loop
// route stops (zero steer) parked on the last point, start_patrol rewinds to
// point 0 and arms the goal, an empty route steers to zero, and two identical
// patrols produce bit-identical steer sequences + an identical cursor
// progression (the lockstep determinism pillar).

#include "ai/Patrol.h"

#include "ai/GridAStar.h"
#include "ai/NavAgent.h"
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

// World centre of a patrol point under `layout` (z*width + x cell index).
math::Vec3 point_world(const PatrolPoint& p, const GridLayout& layout) {
    const u32 cell = p.z * layout.width + p.x;
    return cell_to_world(cell, layout);
}
}  // namespace

TEST_CASE("patrol: an empty route steers to zero", "[ai]") {
    GridAStar grid;
    grid.resize(8, 8);
    const GridLayout layout{1.0f, 0.0f, 0.0f, 8u};

    NavAgent agent;
    agent.layout = layout;
    PatrolRoute route;  // no points

    const math::Vec3 dir =
        update_patrol(agent, route, grid, cell_to_world(0u, layout));
    REQUIRE(dir.x == Catch::Approx(0.0f));
    REQUIRE(dir.y == Catch::Approx(0.0f));
    REQUIRE(dir.z == Catch::Approx(0.0f));
    REQUIRE(route.current == 0u);
}

TEST_CASE("patrol: start_patrol rewinds to point 0 and arms the goal", "[ai]") {
    const GridLayout layout{1.0f, 0.0f, 0.0f, 16u};

    NavAgent agent;
    agent.layout = layout;
    agent.follower.arrival_radius_m = 0.6f;

    PatrolRoute route;
    route.points = {PatrolPoint{2u, 2u}, PatrolPoint{12u, 4u},
                    PatrolPoint{6u, 11u}};
    route.current = 2u;  // pretend we were mid-route

    start_patrol(agent, route);
    REQUIRE(route.current == 0u);          // rewound
    REQUIRE(agent.has_goal);               // goal armed
    REQUIRE_FALSE(agent.has_path);         // set_goal cleared the path
    REQUIRE(agent.goal_x == 2u);           // points[0]
    REQUIRE(agent.goal_z == 2u);
    REQUIRE(patrol_target_cell(route, layout) == 2u * 16u + 2u);
}

TEST_CASE("patrol: a three-point route is visited in order", "[ai]") {
    GridAStar grid;
    grid.resize(24, 24);
    const GridLayout layout{1.0f, 0.0f, 0.0f, 24u};

    NavAgent agent;
    agent.layout = layout;
    agent.follower.arrival_radius_m = 0.6f;

    PatrolRoute route;
    route.loop = false;  // a single pass: end parked on the last point
    route.points = {PatrolPoint{4u, 4u}, PatrolPoint{18u, 6u},
                    PatrolPoint{10u, 20u}};

    start_patrol(agent, route);

    // Walk the bot along each returned steer, recording which point index it is
    // within arrival of, to confirm the sequence 0 -> 1 -> 2.
    math::Vec3 pos = point_world(route.points[0], layout);  // start on point 0
    std::vector<usize> visited_order;
    usize last_seen = 9999u;  // sentinel: no point seen yet
    for (int i = 0; i < 4000; ++i) {
        const math::Vec3 dir = update_patrol(agent, route, grid, pos);

        // Note when the bot is within arrival of any point, in visit order.
        for (usize p = 0; p < route.points.size(); ++p) {
            if (xz_dist(pos, point_world(route.points[p], layout)) <=
                agent.follower.arrival_radius_m) {
                if (p != last_seen) {
                    visited_order.push_back(p);
                    last_seen = p;
                }
            }
        }

        // Parked on the final point: steer has gone to zero, route is done.
        if (route.current == route.points.size() - 1u &&
            dir.x == Catch::Approx(0.0f) && dir.z == Catch::Approx(0.0f) &&
            xz_dist(pos, point_world(route.points.back(), layout)) <=
                agent.follower.arrival_radius_m) {
            break;
        }
        pos = step_along(pos, dir, 0.4f);
    }

    // Every point was reached, strictly in route order 0,1,2.
    REQUIRE(visited_order.size() == 3u);
    REQUIRE(visited_order[0] == 0u);
    REQUIRE(visited_order[1] == 1u);
    REQUIRE(visited_order[2] == 2u);
    REQUIRE(route.current == 2u);  // ended on the last point
}

TEST_CASE("patrol: loop wraps back to point 0 after the last point", "[ai]") {
    GridAStar grid;
    grid.resize(24, 24);
    const GridLayout layout{1.0f, 0.0f, 0.0f, 24u};

    NavAgent agent;
    agent.layout = layout;
    agent.follower.arrival_radius_m = 0.6f;

    PatrolRoute route;
    route.loop = true;
    route.points = {PatrolPoint{4u, 4u}, PatrolPoint{18u, 6u},
                    PatrolPoint{10u, 20u}};

    start_patrol(agent, route);

    // Record the cursor value each tick; with looping the sequence must include
    // the wrap 2 -> 0 (i.e. we see current return to 0 AFTER having reached 2).
    math::Vec3 pos = point_world(route.points[0], layout);
    bool reached_last = false;
    bool wrapped_to_zero = false;
    for (int i = 0; i < 8000; ++i) {
        const math::Vec3 dir = update_patrol(agent, route, grid, pos);
        if (route.current == route.points.size() - 1u) reached_last = true;
        if (reached_last && route.current == 0u) {
            wrapped_to_zero = true;
            break;
        }
        pos = step_along(pos, dir, 0.4f);
    }
    REQUIRE(reached_last);
    REQUIRE(wrapped_to_zero);  // cursor cycled 0->1->2->0
}

TEST_CASE("patrol: a non-loop route stops parked on the last point", "[ai]") {
    GridAStar grid;
    grid.resize(20, 20);
    const GridLayout layout{1.0f, 0.0f, 0.0f, 20u};

    NavAgent agent;
    agent.layout = layout;
    agent.follower.arrival_radius_m = 0.6f;

    PatrolRoute route;
    route.loop = false;
    route.points = {PatrolPoint{3u, 3u}, PatrolPoint{16u, 16u}};

    start_patrol(agent, route);

    // Drive to the end; once parked on the last point the steer must be zero and
    // the cursor must hold at the last index across further ticks.
    math::Vec3 pos = point_world(route.points[0], layout);
    bool stopped = false;
    for (int i = 0; i < 4000; ++i) {
        const math::Vec3 dir = update_patrol(agent, route, grid, pos);
        const bool at_last =
            xz_dist(pos, point_world(route.points.back(), layout)) <=
            agent.follower.arrival_radius_m;
        if (at_last && dir.x == Catch::Approx(0.0f) &&
            dir.z == Catch::Approx(0.0f)) {
            stopped = true;
            break;
        }
        pos = step_along(pos, dir, 0.4f);
    }
    REQUIRE(stopped);
    REQUIRE(route.current == route.points.size() - 1u);  // held on the last point

    // Further updates keep returning zero and the cursor never advances past the
    // last point (no wrap, because loop == false).
    for (int i = 0; i < 8; ++i) {
        const math::Vec3 dir = update_patrol(agent, route, grid, pos);
        REQUIRE(dir.x == Catch::Approx(0.0f));
        REQUIRE(dir.z == Catch::Approx(0.0f));
        REQUIRE(route.current == route.points.size() - 1u);
    }
}

TEST_CASE("patrol: identical patrols produce bit-identical steer sequences",
          "[ai][determinism]") {
    const GridLayout layout{1.0f, 0.0f, 0.0f, 24u};

    const auto build_grid = []() {
        GridAStar g;
        g.resize(24, 24);
        // A partial wall to force a non-trivial route with tie choices.
        for (u32 z = 2; z <= 18; ++z) g.set_blocked(11, z, true);
        return g;
    };

    const auto make = [&](NavAgent& a, PatrolRoute& r) {
        a.layout = layout;
        a.follower.arrival_radius_m = 0.6f;
        r.loop = true;
        r.points = {PatrolPoint{2u, 2u}, PatrolPoint{20u, 5u},
                    PatrolPoint{6u, 21u}};
        start_patrol(a, r);
    };

    GridAStar grid_a = build_grid();
    GridAStar grid_b = build_grid();
    NavAgent a;
    NavAgent b;
    PatrolRoute ra;
    PatrolRoute rb;
    make(a, ra);
    make(b, rb);

    // Walk both in lockstep, advancing each by its own returned steer so cursor
    // advancement is exercised. Steer + cursor must match bit-for-bit every tick.
    math::Vec3 pa = point_world(ra.points[0], layout);
    math::Vec3 pb = pa;
    for (int i = 0; i < 400; ++i) {
        const math::Vec3 da = update_patrol(a, ra, grid_a, pa);
        const math::Vec3 db = update_patrol(b, rb, grid_b, pb);
        REQUIRE(da.x == db.x);
        REQUIRE(da.y == db.y);
        REQUIRE(da.z == db.z);
        REQUIRE(ra.current == rb.current);
        pa = step_along(pa, da, 0.4f);
        pb = step_along(pb, db, 0.4f);
    }
}
