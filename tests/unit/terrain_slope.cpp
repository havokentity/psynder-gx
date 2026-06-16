// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/terrain_slope.cpp — deterministic terrain slope + walkability
// queries (engine/world/outdoor/TerrainSlope): the up-dot (cos of the slope
// angle), the slope angle in radians, and the lockstep-safe walkability gate
// used to validate spawn points and constrain agent pathing on the
// Battlefield-light outdoor map.

#include "world/outdoor/TerrainSlope.h"
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

TEST_CASE("terrain-slope: flat ground is level and fully walkable",
          "[terrain][gameplay]") {
    std::vector<u16> hm(8 * 8, 500u);  // 5 m everywhere (scale 0.01)
    const HeightmapDesc d = make_desc(hm, 8, 8, 1.0f, 0.01f);

    REQUIRE(terrain_slope_updot(d, 3.0f, 3.0f) ==
            Catch::Approx(1.0f).margin(1e-5f));
    REQUIRE(terrain_slope_radians(d, 3.0f, 3.0f) ==
            Catch::Approx(0.0f).margin(1e-5f));
    // cos(45deg) ~ 0.707 threshold — flat ground clears it comfortably.
    REQUIRE(terrain_walkable(d, 3.0f, 3.0f, 0.707f));
}

TEST_CASE("terrain-slope: a steep +X ramp is tilted and not walkable",
          "[terrain][gameplay]") {
    // heights[x] = x*100 -> height = x metres (scale 0.01), spacing 1 m: a
    // 1 m rise per 1 m of run -> a 45deg ramp, updot ~ cos(45deg) ~ 0.707.
    std::vector<u16> hm(8 * 8, 0u);
    for (u32 z = 0; z < 8; ++z)
        for (u32 x = 0; x < 8; ++x) hm[z * 8 + x] = static_cast<u16>(x * 100u);
    const HeightmapDesc d = make_desc(hm, 8, 8, 1.0f, 0.01f);

    const f32 updot = terrain_slope_updot(d, 4.0f, 4.0f);
    REQUIRE(updot < 1.0f);                         // noticeably tilted
    REQUIRE(updot == Catch::Approx(0.70710678f).margin(1e-3f));  // ~cos(45deg)

    REQUIRE(terrain_slope_radians(d, 4.0f, 4.0f) ==
            Catch::Approx(0.7853981f).margin(1e-3f));  // ~pi/4 = 0.785 rad

    // With a strict cos(max_angle) = 0.8 gate (~37deg), the 45deg ramp fails.
    REQUIRE_FALSE(terrain_walkable(d, 4.0f, 4.0f, 0.8f));
}

TEST_CASE("terrain-slope: a gentle ramp stays walkable", "[terrain][gameplay]") {
    // heights[x] = x*20 -> height = 0.2 m per 1 m of run: a shallow ~11deg
    // slope, updot well above a 0.8 gate.
    std::vector<u16> hm(8 * 8, 0u);
    for (u32 z = 0; z < 8; ++z)
        for (u32 x = 0; x < 8; ++x) hm[z * 8 + x] = static_cast<u16>(x * 20u);
    const HeightmapDesc d = make_desc(hm, 8, 8, 1.0f, 0.01f);

    const f32 updot = terrain_slope_updot(d, 4.0f, 4.0f);
    REQUIRE(updot < 1.0f);   // still slightly tilted
    REQUIRE(updot > 0.8f);   // but shallow enough to traverse
    REQUIRE(terrain_walkable(d, 4.0f, 4.0f, 0.8f));
}

TEST_CASE("terrain-slope: repeated queries are bit-identical",
          "[terrain][gameplay][determinism]") {
    std::vector<u16> hm(8 * 8, 0u);
    for (u32 z = 0; z < 8; ++z)
        for (u32 x = 0; x < 8; ++x) hm[z * 8 + x] = static_cast<u16>(x * 100u);
    const HeightmapDesc d = make_desc(hm, 8, 8, 1.0f, 0.01f);

    const f32 a = terrain_slope_updot(d, 3.5f, 2.25f);
    const f32 b = terrain_slope_updot(d, 3.5f, 2.25f);
    REQUIRE(a == b);  // exact: pure algebra, no transcendental on this path
}

TEST_CASE("terrain-slope: walkability gate is the cos-threshold comparison",
          "[terrain]") {
    std::vector<u16> hm(8 * 8, 500u);  // flat
    const HeightmapDesc d = make_desc(hm, 8, 8, 1.0f, 0.01f);

    const f32 updot = terrain_slope_updot(d, 2.0f, 2.0f);
    // The gate is exactly `updot >= min_updot`: equal threshold passes, a hair
    // above it fails.
    REQUIRE(terrain_walkable(d, 2.0f, 2.0f, updot));
    REQUIRE_FALSE(terrain_walkable(d, 2.0f, 2.0f, updot + 1e-3f));
}
