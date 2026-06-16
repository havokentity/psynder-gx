// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/ai_flowfield.cpp — deterministic grid flow-field pathing: the
// gradient points toward the goal, obstacles block + route around (or mark
// unreachable), and the build is bit-reproducible.

#include "ai/FlowField.h"

#include "math/Math.h"

#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace psynder;
using namespace psynder::ai;

namespace {
math::Vec3 cell_center(u32 cx, u32 cz, f32 cell) {
    return {(static_cast<f32>(cx) + 0.5f) * cell, 0.0f,
            (static_cast<f32>(cz) + 0.5f) * cell};
}
}  // namespace

TEST_CASE("ai: flow field gradient points toward the goal in open ground",
          "[ai][flowfield]") {
    FlowField ff;
    ff.resize({0, 0, 0}, 1.0f, 10, 10);
    ff.build(cell_center(9, 9, 1.0f));  // goal in the far +X,+Z corner

    const math::Vec3 d = ff.sample(cell_center(0, 0, 1.0f));  // near origin
    REQUIRE(d.x > 0.0f);  // toward +X (goal)
    REQUIRE(d.z > 0.0f);  // toward +Z (goal)
    REQUIRE(d.y == 0.0f);
    // The goal cell itself has no outward flow.
    const math::Vec3 g = ff.sample(cell_center(9, 9, 1.0f));
    REQUIRE(g.x == 0.0f);
    REQUIRE(g.z == 0.0f);
}

TEST_CASE("ai: a full wall makes the far side unreachable", "[ai][flowfield]") {
    FlowField ff;
    ff.resize({0, 0, 0}, 1.0f, 11, 11);
    // Block the whole x=5 column.
    ff.block_aabb(math::Aabb{{5.0f, 0.0f, 0.0f}, {6.0f, 1.0f, 11.0f}});
    ff.build(cell_center(10, 5, 1.0f));  // goal on the right

    REQUIRE(ff.cost(10, 5) == 0u);                 // goal cell
    REQUIRE(ff.cost(0, 5) == kUnreachable);        // left side cannot reach it
    const math::Vec3 d = ff.sample(cell_center(0, 5, 1.0f));
    REQUIRE(d.x == 0.0f);
    REQUIRE(d.z == 0.0f);
}

TEST_CASE("ai: a wall with a gap routes around it", "[ai][flowfield]") {
    FlowField ff;
    ff.resize({0, 0, 0}, 1.0f, 11, 11);
    // Wall x=5 for z in [0,9]; a gap at z=10 lets a path through.
    ff.block_aabb(math::Aabb{{5.0f, 0.0f, 0.0f}, {6.0f, 1.0f, 10.0f}});
    ff.build(cell_center(10, 5, 1.0f));

    REQUIRE(ff.cost(0, 5) != kUnreachable);  // a path exists (around the top)
    // Just left of the wall, flow must not drive straight +X into the blocked
    // column — it should route along/up toward the gap.
    const math::Vec3 d = ff.sample(cell_center(4, 5, 1.0f));
    REQUIRE((d.x != 0.0f || d.z != 0.0f));  // has a direction
    REQUIRE(d.x <= 0.0f);                   // not pushing into the wall
}

TEST_CASE("ai: flow field build is deterministic", "[ai][flowfield][determinism]") {
    const auto build = []() {
        FlowField ff;
        ff.resize({0, 0, 0}, 1.0f, 16, 16);
        ff.block_aabb(math::Aabb{{6.0f, 0.0f, 2.0f}, {7.0f, 1.0f, 14.0f}});
        ff.block_aabb(math::Aabb{{10.0f, 0.0f, 0.0f}, {11.0f, 1.0f, 12.0f}});
        ff.build(cell_center(15, 15, 1.0f));
        std::vector<u32> costs;
        std::vector<f32> dirs;
        for (u32 z = 0; z < 16; ++z)
            for (u32 x = 0; x < 16; ++x) {
                costs.push_back(ff.cost(x, z));
                const math::Vec3 s = ff.sample(cell_center(x, z, 1.0f));
                dirs.push_back(s.x);
                dirs.push_back(s.z);
            }
        return std::pair<std::vector<u32>, std::vector<f32>>{costs, dirs};
    };
    REQUIRE(build() == build());
}
