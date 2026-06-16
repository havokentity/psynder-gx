// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/ai_waypoint_graph.cpp — sparse navigation waypoint graph:
// nearest-node lookup with a tie-break, a linear-chain A* through-path, the
// shorter of two competing routes, an unreachable disconnected goal, a
// single-node start == goal path, node accessors, and bit-reproducible paths.

#include "ai/WaypointGraph.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

using namespace psynder;
using namespace psynder::ai;

TEST_CASE("waypoint graph: nodes and edges report basic accessors", "[ai]") {
    WaypointGraph g;
    const u32 a = g.add_node(math::Vec3{0.0f, 0.0f, 0.0f});
    const u32 b = g.add_node(math::Vec3{3.0f, 0.0f, 4.0f});

    REQUIRE(a == 0u);
    REQUIRE(b == 1u);
    REQUIRE(g.node_count() == 2u);

    REQUIRE(g.node_pos(a).x == Catch::Approx(0.0f));
    REQUIRE(g.node_pos(b).x == Catch::Approx(3.0f));
    REQUIRE(g.node_pos(b).z == Catch::Approx(4.0f));

    // Out-of-range node_pos is the zero vector.
    const math::Vec3 oob = g.node_pos(99u);
    REQUIRE(oob.x == Catch::Approx(0.0f));
    REQUIRE(oob.y == Catch::Approx(0.0f));
    REQUIRE(oob.z == Catch::Approx(0.0f));
}

TEST_CASE("waypoint graph: nearest node returns the closest with a tie-break",
          "[ai]") {
    WaypointGraph g;
    g.add_node(math::Vec3{0.0f, 0.0f, 0.0f});   // 0
    g.add_node(math::Vec3{10.0f, 0.0f, 0.0f});  // 1
    g.add_node(math::Vec3{5.0f, 0.0f, 0.0f});   // 2

    u32 nearest = 99u;
    REQUIRE(g.nearest_node(math::Vec3{9.0f, 0.0f, 0.0f}, nearest));
    REQUIRE(nearest == 1u);

    REQUIRE(g.nearest_node(math::Vec3{4.0f, 0.0f, 0.0f}, nearest));
    REQUIRE(nearest == 2u);

    // Equidistant between node 0 (0,0,0) and node 2 (5,0,0) -> the lower index
    // wins (0). Both sit sqrt(7.25) from (2.5,0,1); node 1 at x=10 is farther.
    REQUIRE(g.nearest_node(math::Vec3{2.5f, 0.0f, 1.0f}, nearest));
    REQUIRE(nearest == 0u);

    // An empty graph reports no nearest node.
    WaypointGraph empty;
    u32 sentinel = 7u;
    REQUIRE_FALSE(empty.nearest_node(math::Vec3{0.0f, 0.0f, 0.0f}, sentinel));
    REQUIRE(sentinel == 7u);  // left untouched on failure
}

TEST_CASE("waypoint graph: a linear chain returns the through path", "[ai]") {
    WaypointGraph g;
    g.add_node(math::Vec3{0.0f, 0.0f, 0.0f});   // 0
    g.add_node(math::Vec3{1.0f, 0.0f, 0.0f});   // 1
    g.add_node(math::Vec3{2.0f, 0.0f, 0.0f});   // 2
    g.add_node(math::Vec3{3.0f, 0.0f, 0.0f});   // 3
    g.add_edge(0, 1);
    g.add_edge(1, 2);
    g.add_edge(2, 3);

    std::vector<u32> path;
    REQUIRE(g.find_path(0, 3, path));
    REQUIRE(path.size() == 4u);
    REQUIRE(path.front() == 0u);
    REQUIRE(path.back() == 3u);
    REQUIRE(path[1] == 1u);
    REQUIRE(path[2] == 2u);
}

TEST_CASE("waypoint graph: two routes returns the shorter one", "[ai]") {
    // 0 --- 1 --- 2 --- 3   (long detour: y = +5 corridor)
    //  \                 /
    //   ------ 4 -------     (short hop straight across)
    WaypointGraph g;
    g.add_node(math::Vec3{0.0f, 0.0f, 0.0f});    // 0 start
    g.add_node(math::Vec3{0.0f, 0.0f, 5.0f});    // 1 detour
    g.add_node(math::Vec3{10.0f, 0.0f, 5.0f});   // 2 detour
    g.add_node(math::Vec3{10.0f, 0.0f, 0.0f});   // 3 goal
    g.add_node(math::Vec3{5.0f, 0.0f, 0.0f});    // 4 mid (short route)

    // Long route 0 -> 1 -> 2 -> 3  (total = 5 + 10 + 5 = 20).
    g.add_edge(0, 1);
    g.add_edge(1, 2);
    g.add_edge(2, 3);
    // Short route 0 -> 4 -> 3  (total = 5 + 5 = 10).
    g.add_edge(0, 4);
    g.add_edge(4, 3);

    std::vector<u32> path;
    REQUIRE(g.find_path(0, 3, path));
    REQUIRE(path.size() == 3u);
    REQUIRE(path.front() == 0u);
    REQUIRE(path[1] == 4u);  // took the short hop, not the detour
    REQUIRE(path.back() == 3u);
}

TEST_CASE("waypoint graph: a disconnected goal is unreachable", "[ai]") {
    WaypointGraph g;
    g.add_node(math::Vec3{0.0f, 0.0f, 0.0f});   // 0
    g.add_node(math::Vec3{1.0f, 0.0f, 0.0f});   // 1
    g.add_node(math::Vec3{50.0f, 0.0f, 0.0f});  // 2 island
    g.add_edge(0, 1);
    // Node 2 has no edges -> unreachable from {0,1}.

    std::vector<u32> path;
    REQUIRE_FALSE(g.find_path(0, 2, path));
    REQUIRE(path.empty());

    // An out-of-range endpoint also fails with an empty path.
    REQUIRE_FALSE(g.find_path(0, 99u, path));
    REQUIRE(path.empty());
    REQUIRE_FALSE(g.find_path(99u, 0, path));
    REQUIRE(path.empty());
}

TEST_CASE("waypoint graph: start equals goal gives a single-node path", "[ai]") {
    WaypointGraph g;
    g.add_node(math::Vec3{2.0f, 0.0f, 2.0f});  // 0
    g.add_node(math::Vec3{4.0f, 0.0f, 4.0f});  // 1
    g.add_edge(0, 1);

    std::vector<u32> path;
    REQUIRE(g.find_path(1, 1, path));
    REQUIRE(path.size() == 1u);
    REQUIRE(path.front() == 1u);
}

TEST_CASE("waypoint graph: one-directional edge is honoured", "[ai]") {
    WaypointGraph g;
    g.add_node(math::Vec3{0.0f, 0.0f, 0.0f});  // 0
    g.add_node(math::Vec3{1.0f, 0.0f, 0.0f});  // 1
    g.add_edge(0, 1, false);  // forward only

    std::vector<u32> fwd;
    REQUIRE(g.find_path(0, 1, fwd));
    REQUIRE(fwd.size() == 2u);

    std::vector<u32> back;
    REQUIRE_FALSE(g.find_path(1, 0, back));  // no reverse edge
    REQUIRE(back.empty());
}

TEST_CASE("waypoint graph: pathfinding is deterministic", "[ai]") {
    const auto build = []() {
        WaypointGraph g;
        // A diamond lattice with several equal-length competing routes to force
        // tie choices in the heap.
        for (u32 i = 0; i < 9; ++i) {
            const f32 fx = static_cast<f32>(i % 3);
            const f32 fz = static_cast<f32>(i / 3);
            g.add_node(math::Vec3{fx, 0.0f, fz});
        }
        // 4-connected grid edges (bidirectional, unit cost each).
        for (u32 z = 0; z < 3; ++z) {
            for (u32 x = 0; x < 3; ++x) {
                const u32 here = z * 3 + x;
                if (x + 1 < 3) g.add_edge(here, here + 1);
                if (z + 1 < 3) g.add_edge(here, here + 3);
            }
        }
        return g;
    };

    WaypointGraph g1 = build();
    WaypointGraph g2 = build();

    std::vector<u32> first;
    std::vector<u32> second;
    REQUIRE(g1.find_path(0, 8, first));
    REQUIRE(g2.find_path(0, 8, second));
    REQUIRE_FALSE(first.empty());
    REQUIRE(first == second);  // bit-identical path on a re-run
}
