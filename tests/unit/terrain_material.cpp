// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/terrain_material.cpp — deterministic terrain SURFACE MATERIAL
// classification (engine/world/outdoor/TerrainMaterial): grass / rock / snow /
// sand chosen from a point's STEEPNESS and ELEVATION, plus the per-material
// movement-friction multiplier. Steepness takes priority over elevation (a
// cliff is rock at any height). Pure algebra over the lockstep-safe up-dot +
// height queries, so the same serialized terrain yields bit-identical results.

#include "world/outdoor/TerrainMaterial.h"
#include "world/outdoor/Terrain.h"
#include "world/outdoor/HeightfieldQuery.h"  // terrain_height (sanity asserts)
#include "world/outdoor/TerrainSlope.h"      // terrain_slope_updot (sanity asserts)

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

TEST_CASE("terrain-material: flat low-mid field is grass with default bands",
          "[terrain][gameplay]") {
    // scale 0.01 -> u16 1000 == 10 m. Flat (updot ~ 1, clears rock gate),
    // 10 m sits above the 2 m sand band and below the 60 m snow band -> Grass.
    std::vector<u16> hm(8 * 8, 1000u);
    const HeightmapDesc d = make_desc(hm, 8, 8, 1.0f, 0.01f);

    REQUIRE(terrain_material(d, 4.0f, 4.0f, kDefaultMaterialBands) ==
            SurfaceMaterial::Grass);
}

TEST_CASE("terrain-material: a steep ramp is rock even when low",
          "[terrain][gameplay]") {
    // heights[x] = x*100 -> 1 m rise per 1 m run (scale 0.01, spacing 1 m): a
    // 45deg ramp, updot ~ cos(45deg) ~ 0.707. Bands with rock_max_updot 0.8
    // (~37deg gate) make this steep slope Rock. Elevation here is low/mid
    // (~4 m at the sample point) and would otherwise be Grass — steepness wins.
    std::vector<u16> hm(8 * 8, 0u);
    for (u32 z = 0; z < 8; ++z)
        for (u32 x = 0; x < 8; ++x) hm[z * 8 + x] = static_cast<u16>(x * 100u);
    const HeightmapDesc d = make_desc(hm, 8, 8, 1.0f, 0.01f);

    const MaterialBands b{/*rock_max_updot=*/0.8f, /*snow_min=*/60.0f,
                          /*sand_max=*/2.0f};
    REQUIRE(terrain_material(d, 4.0f, 4.0f, b) == SurfaceMaterial::Rock);
}

TEST_CASE("terrain-material: high flat field is snow", "[terrain][gameplay]") {
    // scale 0.01 -> u16 8000 == 80 m, flat. 80 m >= the 60 m snow line -> Snow.
    std::vector<u16> hm(8 * 8, 8000u);
    const HeightmapDesc d = make_desc(hm, 8, 8, 1.0f, 0.01f);

    REQUIRE(terrain_material(d, 4.0f, 4.0f, kDefaultMaterialBands) ==
            SurfaceMaterial::Snow);
}

TEST_CASE("terrain-material: very low flat field is sand",
          "[terrain][gameplay]") {
    // scale 0.01 -> u16 50 == 0.5 m, flat. 0.5 m <= the 2 m sand band -> Sand.
    std::vector<u16> hm(8 * 8, 50u);
    const HeightmapDesc d = make_desc(hm, 8, 8, 1.0f, 0.01f);

    REQUIRE(terrain_material(d, 4.0f, 4.0f, kDefaultMaterialBands) ==
            SurfaceMaterial::Sand);
}

TEST_CASE("terrain-material: steepness takes priority over elevation",
          "[terrain][gameplay]") {
    // A steep HIGH slope must classify as Rock (a bare cliff), NOT Snow, even
    // though its elevation is above the snow line. heights[x] = base + x*100
    // gives a 45deg ramp (updot ~ 0.707) whose sampled height is well above
    // 60 m. With rock_max_updot 0.8 the steepness gate fires first.
    std::vector<u16> hm(8 * 8, 0u);
    for (u32 z = 0; z < 8; ++z)
        for (u32 x = 0; x < 8; ++x)
            hm[z * 8 + x] = static_cast<u16>(7000u + x * 100u);  // 70..77 m
    const HeightmapDesc d = make_desc(hm, 8, 8, 1.0f, 0.01f);

    const MaterialBands b{/*rock_max_updot=*/0.8f, /*snow_min=*/60.0f,
                          /*sand_max=*/2.0f};

    // Sanity: the sampled point IS above the snow line, so without the
    // steepness rule it would be Snow.
    REQUIRE(terrain_height(d, 4.0f, 4.0f) > b.snow_min_height_m);
    // But the slope is steep enough to trip the rock gate -> Rock wins.
    REQUIRE(terrain_slope_updot(d, 4.0f, 4.0f) < b.rock_max_updot);
    REQUIRE(terrain_material(d, 4.0f, 4.0f, b) == SurfaceMaterial::Rock);
}

TEST_CASE("terrain-material: friction is the documented per-material multiplier",
          "[terrain][gameplay]") {
    REQUIRE(material_friction(SurfaceMaterial::Grass) ==
            Catch::Approx(1.0f).margin(1e-6f));
    REQUIRE(material_friction(SurfaceMaterial::Rock) ==
            Catch::Approx(1.0f).margin(1e-6f));
    REQUIRE(material_friction(SurfaceMaterial::Snow) ==
            Catch::Approx(0.6f).margin(1e-6f));
    REQUIRE(material_friction(SurfaceMaterial::Sand) ==
            Catch::Approx(0.8f).margin(1e-6f));
    // Snow is the slipperiest; Grass/Rock have full grip.
    REQUIRE(material_friction(SurfaceMaterial::Snow) <
            material_friction(SurfaceMaterial::Sand));
    REQUIRE(material_friction(SurfaceMaterial::Sand) <
            material_friction(SurfaceMaterial::Grass));
}

TEST_CASE("terrain-material: classification is bit-identical across repeats",
          "[terrain][gameplay][determinism]") {
    std::vector<u16> hm(8 * 8, 0u);
    for (u32 z = 0; z < 8; ++z)
        for (u32 x = 0; x < 8; ++x) hm[z * 8 + x] = static_cast<u16>(x * 100u);
    const HeightmapDesc d = make_desc(hm, 8, 8, 1.0f, 0.01f);

    const SurfaceMaterial a = terrain_material(d, 3.5f, 2.25f,
                                               kDefaultMaterialBands);
    const SurfaceMaterial b = terrain_material(d, 3.5f, 2.25f,
                                               kDefaultMaterialBands);
    REQUIRE(a == b);  // exact: pure algebra, no transcendental on this path
}
