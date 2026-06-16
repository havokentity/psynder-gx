// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/terrain_spawn.cpp — deterministic terrain-aware spawn selection
// (engine/world/outdoor/TerrainSpawn): farthest-from-enemies XZ pick plus a
// ground-clamp onto the heightmap. The spawn side of the Battlefield-light
// outdoor map.

#include "world/outdoor/TerrainSpawn.h"
#include "world/outdoor/HeightfieldQuery.h"
#include "world/outdoor/Terrain.h"

#include "math/Math.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cstdint>
#include <span>
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
}  // namespace

TEST_CASE("terrain-spawn: farthest-from-enemy pick favours the distant candidate",
          "[terrain][gameplay]") {
    const std::vector<math::Vec3> candidates{
        {0.0f, 0.0f, 0.0f}, {10.0f, 0.0f, 0.0f}, {20.0f, 0.0f, 0.0f}};
    const std::vector<math::Vec3> enemies{{0.0f, 0.0f, 0.0f}};  // enemy at x=0

    const usize idx = select_farthest_spawn(candidates, enemies);
    REQUIRE(idx == 2);  // x=20 is farthest from the enemy at x=0
}

TEST_CASE("terrain-spawn: no enemies falls back to index 0", "[terrain][gameplay]") {
    const std::vector<math::Vec3> candidates{
        {0.0f, 0.0f, 0.0f}, {10.0f, 0.0f, 0.0f}, {20.0f, 0.0f, 0.0f}};
    const std::span<const math::Vec3> no_enemies{};

    REQUIRE(select_farthest_spawn(candidates, no_enemies) == 0);
}

TEST_CASE("terrain-spawn: a distance tie breaks to the lowest index",
          "[terrain][gameplay]") {
    // Enemy at x=15: candidates at x=10 and x=20 are each 5 m away (tie); the
    // candidate at x=0 is 15 m away and therefore strictly farthest.
    const std::vector<math::Vec3> candidates{
        {0.0f, 0.0f, 0.0f}, {10.0f, 0.0f, 0.0f}, {20.0f, 0.0f, 0.0f}};
    const std::vector<math::Vec3> enemies{{15.0f, 0.0f, 0.0f}};

    REQUIRE(select_farthest_spawn(candidates, enemies) == 0);  // x=0 wins
}

TEST_CASE("terrain-spawn: empty candidates yields index 0", "[terrain][gameplay]") {
    const std::span<const math::Vec3> none{};
    const std::vector<math::Vec3> enemies{{0.0f, 0.0f, 0.0f}};
    REQUIRE(select_farthest_spawn(none, enemies) == 0);
}

TEST_CASE("terrain-spawn: Y ignored — only XZ distance decides the pick",
          "[terrain][gameplay]") {
    // The x=20 candidate sits high in Y; the x=0 candidate shares the enemy's
    // XZ. XZ-only distance must still pick x=20 regardless of the Y values.
    const std::vector<math::Vec3> candidates{
        {0.0f, 100.0f, 0.0f}, {20.0f, -50.0f, 0.0f}};
    const std::vector<math::Vec3> enemies{{0.0f, 0.0f, 0.0f}};

    REQUIRE(select_farthest_spawn(candidates, enemies) == 1);
}

TEST_CASE("terrain-spawn: chosen point clamps onto a flat heightmap",
          "[terrain][gameplay]") {
    std::vector<u16> hm(8 * 8, 500u);  // 500 * 0.01 = 5 m everywhere
    const HeightmapDesc d = make_desc(hm, 8, 8, 1.0f, 0.01f);

    const std::vector<math::Vec3> candidates{
        {1.0f, 99.0f, 1.0f}, {3.0f, 99.0f, 2.0f}, {5.0f, 99.0f, 4.0f}};
    const std::vector<math::Vec3> enemies{{1.0f, 0.0f, 1.0f}};

    const math::Vec3 p = pick_terrain_spawn(d, candidates, enemies, /*foot=*/0.9f);
    // Farthest from the enemy at (1,1) is (5,4) -> clamped to 5 m + foot.
    REQUIRE(p.x == Catch::Approx(5.0f));
    REQUIRE(p.z == Catch::Approx(4.0f));
    REQUIRE(p.y == Catch::Approx(5.9f));  // 5 m surface + 0.9 m foot offset
}

TEST_CASE("terrain-spawn: chosen point clamps onto a +X ramp surface",
          "[terrain][gameplay]") {
    // heights[x] = x*100 -> height = x metres (height_scale 0.01).
    std::vector<u16> hm(8 * 8, 0u);
    for (u32 z = 0; z < 8; ++z)
        for (u32 x = 0; x < 8; ++x) hm[z * 8 + x] = static_cast<u16>(x * 100u);
    const HeightmapDesc d = make_desc(hm, 8, 8, 1.0f, 0.01f);

    const std::vector<math::Vec3> candidates{
        {2.0f, 99.0f, 1.0f}, {6.0f, 99.0f, 3.0f}};
    const std::vector<math::Vec3> enemies{{2.0f, 0.0f, 1.0f}};

    const math::Vec3 p = pick_terrain_spawn(d, candidates, enemies);
    // Farther candidate is (6,3); its Y must match the terrain height there.
    REQUIRE(p.x == Catch::Approx(6.0f));
    REQUIRE(p.z == Catch::Approx(3.0f));
    REQUIRE(p.y == Catch::Approx(terrain_height(d, p.x, p.z)));
    REQUIRE(p.y == Catch::Approx(6.0f));  // x=6 -> 6 m on the ramp
}

TEST_CASE("terrain-spawn: empty candidates yields the origin point",
          "[terrain][gameplay]") {
    std::vector<u16> hm(8 * 8, 500u);
    const HeightmapDesc d = make_desc(hm, 8, 8, 1.0f, 0.01f);
    const std::span<const math::Vec3> none{};
    const std::vector<math::Vec3> enemies{{0.0f, 0.0f, 0.0f}};

    const math::Vec3 p = pick_terrain_spawn(d, none, enemies);
    REQUIRE(p.x == Catch::Approx(0.0f));
    REQUIRE(p.y == Catch::Approx(0.0f));
    REQUIRE(p.z == Catch::Approx(0.0f));
}
