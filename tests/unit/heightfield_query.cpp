// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/heightfield_query.cpp — deterministic CPU terrain queries
// (engine/world/outdoor/HeightfieldQuery): bilinear height, surface normal, a
// downward raycast, ground-clamp, and reproducible procedural generation. The
// gameplay/physics side of the Battlefield-light outdoor map.

#include "world/outdoor/HeightfieldQuery.h"
#include "world/outdoor/Terrain.h"

#include "math/Math.h"

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
}  // namespace

TEST_CASE("terrain: flat heightfield — constant height, up normal, clamp",
          "[terrain][gameplay]") {
    std::vector<u16> hm(8 * 8, 500u);  // 500 * 0.01 = 5 m everywhere
    const HeightmapDesc d = make_desc(hm, 8, 8, 1.0f, 0.01f);

    REQUIRE(terrain_height(d, 3.0f, 3.0f) == Catch::Approx(5.0f));
    REQUIRE(terrain_height(d, 3.5f, 2.25f) == Catch::Approx(5.0f));  // bilinear flat

    const math::Vec3 n = terrain_normal(d, 3.0f, 3.0f);
    REQUIRE(n.x == Catch::Approx(0.0f).margin(1e-5f));
    REQUIRE(n.y == Catch::Approx(1.0f).margin(1e-5f));
    REQUIRE(n.z == Catch::Approx(0.0f).margin(1e-5f));

    const math::Vec3 c = clamp_to_ground(d, {3.0f, 99.0f, 3.0f}, /*foot=*/0.9f);
    REQUIRE(c.y == Catch::Approx(5.9f));
}

TEST_CASE("terrain: a +X ramp interpolates height and tilts the normal",
          "[terrain][gameplay]") {
    // heights[x] = x*100 -> height = x metres (height_scale 0.01).
    std::vector<u16> hm(8 * 8, 0u);
    for (u32 z = 0; z < 8; ++z)
        for (u32 x = 0; x < 8; ++x) hm[z * 8 + x] = static_cast<u16>(x * 100u);
    const HeightmapDesc d = make_desc(hm, 8, 8, 1.0f, 0.01f);

    REQUIRE(terrain_height(d, 2.0f, 4.0f) == Catch::Approx(2.0f));
    REQUIRE(terrain_height(d, 2.5f, 4.0f) == Catch::Approx(2.5f));  // linear interp

    // Surface rises toward +X, so the normal leans toward -X (and stays up).
    const math::Vec3 n = terrain_normal(d, 4.0f, 4.0f);
    REQUIRE(n.x < 0.0f);
    REQUIRE(n.y > 0.0f);
    const f32 len = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
    REQUIRE(len == Catch::Approx(1.0f).margin(1e-5f));
}

TEST_CASE("terrain: a downward ray hits the surface; an upward ray misses",
          "[terrain][gameplay]") {
    std::vector<u16> hm(16 * 16, 500u);  // flat 5 m
    const HeightmapDesc d = make_desc(hm, 16, 16, 1.0f, 0.01f);

    const RayHit down =
        terrain_raycast(d, {6.0f, 20.0f, 6.0f}, {0.0f, -1.0f, 0.0f}, 50.0f);
    REQUIRE(down.hit);
    REQUIRE(down.point.y == Catch::Approx(5.0f).margin(1e-2f));
    REQUIRE(down.t == Catch::Approx(15.0f).margin(1e-2f));  // 20 - 5

    const RayHit up =
        terrain_raycast(d, {6.0f, 20.0f, 6.0f}, {0.0f, 1.0f, 0.0f}, 50.0f);
    REQUIRE_FALSE(up.hit);

    // A ray starting below the surface registers an immediate hit.
    const RayHit under =
        terrain_raycast(d, {6.0f, 1.0f, 6.0f}, {0.0f, -1.0f, 0.0f}, 50.0f);
    REQUIRE(under.hit);
    REQUIRE(under.t == Catch::Approx(0.0f));
}

TEST_CASE("terrain: an oblique ray hits a slope near the analytic crossing",
          "[terrain][gameplay]") {
    // heights[x] = x*100 -> height = x metres.
    std::vector<u16> hm(16 * 16, 0u);
    for (u32 z = 0; z < 16; ++z)
        for (u32 x = 0; x < 16; ++x) hm[z * 16 + x] = static_cast<u16>(x * 100u);
    const HeightmapDesc d = make_desc(hm, 16, 16, 1.0f, 0.01f);

    // From (0, 5, 5) aim +X and down: y(t)=5-..., terrain rises as x grows; they
    // cross where 5 - t*sin = x = t*cos. Just assert a plausible on-surface hit.
    const RayHit r =
        terrain_raycast(d, {0.0f, 5.0f, 5.0f}, {1.0f, -1.0f, 0.0f}, 30.0f);
    REQUIRE(r.hit);
    // On the surface: the hit's Y equals the terrain height at the hit XZ.
    REQUIRE(r.point.y == Catch::Approx(terrain_height(d, r.point.x, r.point.z))
                             .margin(1e-2f));
}

TEST_CASE("terrain: procedural hills are reproducible and queryable",
          "[terrain][gameplay][determinism]") {
    std::vector<u16> a, b;
    generate_hills(a, 32, 32, /*seed=*/42u);
    generate_hills(b, 32, 32, /*seed=*/42u);
    REQUIRE(a == b);  // same seed/args -> identical map (within a platform)

    std::vector<u16> c;
    generate_hills(c, 32, 32, /*seed=*/7u);
    REQUIRE(a != c);  // a different seed yields different terrain

    const HeightmapDesc d = make_desc(a, 32, 32, 2.0f, 0.02f);
    const f32 hgt = terrain_height(d, 30.0f, 30.0f);
    REQUIRE(hgt >= 0.0f);
    REQUIRE(hgt < 200.0f);  // bounded by amplitude * scale
    const math::Vec3 n = terrain_normal(d, 30.0f, 30.0f);
    const f32 len = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
    REQUIRE(len == Catch::Approx(1.0f).margin(1e-4f));
}

TEST_CASE("terrain: queries outside the map return zero height", "[terrain]") {
    std::vector<u16> hm(4 * 4, 1000u);
    const HeightmapDesc d = make_desc(hm, 4, 4, 1.0f, 0.01f);
    REQUIRE(terrain_height(d, -10.0f, -10.0f) == Catch::Approx(0.0f));
    REQUIRE(terrain_height(d, 100.0f, 100.0f) == Catch::Approx(0.0f));
}
