// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/terrain_path_cost.cpp — slope-weighted terrain traversal cost
// (engine/world/outdoor/TerrainPathCost): the per-node movement-cost multiplier
// (1 on flat ground, rising toward max_cost_mult on steeper walkable ground,
// kImpassableCost past the walkable gate), the cheap passability reject, and the
// distance-scaled edge cost used to weight outdoor A* pathfinding on the
// Battlefield-light map.

#include "world/outdoor/TerrainPathCost.h"
#include "world/outdoor/TerrainSlope.h"
#include "world/outdoor/Terrain.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cstdint>
#include <vector>

using namespace psynder;
using namespace psynder::world::outdoor;

namespace {
HeightmapDesc make_desc(const std::vector<u16>& heights, u32 sx, u32 sz,
                        f32 spacing, f32 scale) {
    HeightmapDesc d{};
    d.size_x = sx;
    d.size_z = sz;
    d.spacing = spacing;
    d.height_scale = scale;
    d.heights = heights.data();
    return d;
}

// Flat field: 5 m everywhere (scale 0.01).
std::vector<u16> make_flat() { return std::vector<u16>(8 * 8, 500u); }

// +X ramp: heights[x] = x * step (units). step=100 with scale 0.01, spacing 1 m
// gives a 1 m rise per 1 m run -> a ~45deg ramp (updot ~ cos(45deg) ~ 0.707).
// A smaller step gives a gentler, still-walkable slope.
std::vector<u16> make_ramp(u16 step) {
    std::vector<u16> hm(8 * 8, 0u);
    for (u32 z = 0; z < 8; ++z)
        for (u32 x = 0; x < 8; ++x)
            hm[z * 8 + x] = static_cast<u16>(x * step);
    return hm;
}
}  // namespace

TEST_CASE("terrain-path-cost: flat ground is the cheapest, unit cost",
          "[terrain][gameplay]") {
    const std::vector<u16> hm = make_flat();
    const HeightmapDesc d = make_desc(hm, 8, 8, 1.0f, 0.01f);

    const f32 cost = terrain_move_cost(d, 3.0f, 3.0f, 0.707f, 5.0f);
    REQUIRE(cost == Catch::Approx(1.0f).margin(1e-5f));
    REQUIRE(terrain_passable(d, 3.0f, 3.0f, 0.707f));
}

TEST_CASE("terrain-path-cost: a moderate walkable slope costs more than flat",
          "[terrain][gameplay]") {
    // Gentle ~11deg ramp (step 20): updot well above a 0.8 gate, so walkable,
    // but tilted -> cost strictly between 1 and max_cost_mult.
    const std::vector<u16> hm = make_ramp(20u);
    const HeightmapDesc d = make_desc(hm, 8, 8, 1.0f, 0.01f);

    const f32 max_mult = 5.0f;
    const f32 updot = terrain_slope_updot(d, 4.0f, 4.0f);
    REQUIRE(updot > 0.8f);  // shallow enough to traverse under a 0.8 gate

    const f32 cost = terrain_move_cost(d, 4.0f, 4.0f, 0.8f, max_mult);
    REQUIRE(cost > 1.0f);
    REQUIRE(cost <= max_mult);
    REQUIRE(terrain_passable(d, 4.0f, 4.0f, 0.8f));

    // Matches the documented linear ramp exactly (pure algebra).
    const f32 expected = 1.0f + (max_mult - 1.0f) * (1.0f - updot) / (1.0f - 0.8f);
    REQUIRE(cost == Catch::Approx(expected).margin(1e-5f));
}

TEST_CASE("terrain-path-cost: steeper ground costs more than gentler ground",
          "[terrain][gameplay]") {
    const HeightmapDesc gentle =
        make_desc(*(new std::vector<u16>(make_ramp(20u))), 8, 8, 1.0f, 0.01f);
    // NOTE: avoid leaks in real tests; keep both vectors alive on the stack.
    (void)gentle;

    const std::vector<u16> hm_gentle = make_ramp(20u);  // ~11deg
    const std::vector<u16> hm_steep = make_ramp(60u);   // ~31deg
    const HeightmapDesc dg = make_desc(hm_gentle, 8, 8, 1.0f, 0.01f);
    const HeightmapDesc ds = make_desc(hm_steep, 8, 8, 1.0f, 0.01f);

    const f32 cg = terrain_move_cost(dg, 4.0f, 4.0f, 0.5f, 5.0f);
    const f32 cs = terrain_move_cost(ds, 4.0f, 4.0f, 0.5f, 5.0f);
    REQUIRE(cg > 1.0f);
    REQUIRE(cs > cg);  // steeper ground is more expensive
}

TEST_CASE("terrain-path-cost: ground past the gate is impassable",
          "[terrain][gameplay]") {
    // 45deg ramp (step 100): updot ~ 0.707, fails a strict 0.8 gate (~37deg).
    const std::vector<u16> hm = make_ramp(100u);
    const HeightmapDesc d = make_desc(hm, 8, 8, 1.0f, 0.01f);

    REQUIRE_FALSE(terrain_passable(d, 4.0f, 4.0f, 0.8f));
    REQUIRE(terrain_move_cost(d, 4.0f, 4.0f, 0.8f, 5.0f) == kImpassableCost);
    REQUIRE(kImpassableCost > 1.0e29f);  // a very large sentinel, not ~costs
}

TEST_CASE("terrain-path-cost: edge cost scales with horizontal distance",
          "[terrain][gameplay]") {
    const std::vector<u16> hm = make_flat();
    const HeightmapDesc d = make_desc(hm, 8, 8, 1.0f, 0.01f);

    // Flat ground -> both endpoints cost 1.0, so the edge cost is exactly the
    // horizontal distance between A and B.
    const f32 e1 = terrain_edge_cost(d, 1.0f, 1.0f, 2.0f, 1.0f, 0.707f, 5.0f);
    REQUIRE(e1 == Catch::Approx(1.0f).margin(1e-5f));  // 1 m apart

    const f32 e3 = terrain_edge_cost(d, 1.0f, 1.0f, 4.0f, 1.0f, 0.707f, 5.0f);
    REQUIRE(e3 == Catch::Approx(3.0f).margin(1e-5f));  // 3 m apart

    // A 3-4-5 right triangle in X/Z -> distance 5 m. Use INTERIOR points (not the
    // map corner, where terrain_normal's central difference samples off-map and
    // fabricates an edge cliff that would read as impassable).
    const f32 ediag = terrain_edge_cost(d, 1.0f, 1.0f, 4.0f, 5.0f, 0.707f, 5.0f);
    REQUIRE(ediag == Catch::Approx(5.0f).margin(1e-5f));
}

TEST_CASE("terrain-path-cost: edge cost averages the two endpoint costs",
          "[terrain][gameplay]") {
    // +X ramp so the two endpoints sit on different steepness.
    const std::vector<u16> hm = make_ramp(20u);  // gentle, fully walkable
    const HeightmapDesc d = make_desc(hm, 8, 8, 1.0f, 0.01f);

    const f32 ax = 2.0f, az = 4.0f;
    const f32 bx = 5.0f, bz = 4.0f;  // 3 m apart along +X
    const f32 min_updot = 0.5f, max_mult = 5.0f;

    const f32 ca = terrain_move_cost(d, ax, az, min_updot, max_mult);
    const f32 cb = terrain_move_cost(d, bx, bz, min_updot, max_mult);
    const f32 dist = 3.0f;  // |bx - ax|, same Z
    const f32 expected = dist * 0.5f * (ca + cb);

    const f32 edge = terrain_edge_cost(d, ax, az, bx, bz, min_updot, max_mult);
    REQUIRE(edge == Catch::Approx(expected).margin(1e-4f));
}

TEST_CASE("terrain-path-cost: an edge touching impassable ground is impassable",
          "[terrain][gameplay]") {
    // 45deg ramp: with a strict 0.8 gate the ramp ground is not walkable.
    const std::vector<u16> hm = make_ramp(100u);
    const HeightmapDesc d = make_desc(hm, 8, 8, 1.0f, 0.01f);

    // Endpoint B sits on the steep ramp -> the whole edge is impassable, even
    // though endpoint A might individually pass.
    const f32 edge =
        terrain_edge_cost(d, 4.0f, 4.0f, 5.0f, 4.0f, 0.8f, 5.0f);
    REQUIRE(edge == kImpassableCost);
}

TEST_CASE("terrain-path-cost: costs are bit-identical on repeat (determinism)",
          "[terrain][gameplay][determinism]") {
    const std::vector<u16> hm = make_ramp(40u);
    const HeightmapDesc d = make_desc(hm, 8, 8, 1.0f, 0.01f);

    const f32 m1 = terrain_move_cost(d, 3.5f, 2.25f, 0.6f, 4.0f);
    const f32 m2 = terrain_move_cost(d, 3.5f, 2.25f, 0.6f, 4.0f);
    REQUIRE(m1 == m2);  // exact: pure algebra, no transcendental on the path

    const f32 e1 = terrain_edge_cost(d, 1.5f, 2.5f, 4.5f, 3.5f, 0.6f, 4.0f);
    const f32 e2 = terrain_edge_cost(d, 1.5f, 2.5f, 4.5f, 3.5f, 0.6f, 4.0f);
    REQUIRE(e1 == e2);  // sqrt is correctly-rounded -> repeatable
}
