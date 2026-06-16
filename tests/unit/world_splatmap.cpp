// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/world_splatmap.cpp — continuous terrain material BLEND WEIGHTS
// (engine/world/outdoor/Splatmap): the SOFT cross-fade complement to
// TerrainMaterial's hard pick. `terrain_splat` returns grass/rock/snow/sand
// weights (each in [0,1], summing to ~1) from a point's STEEPNESS and
// ELEVATION via polynomial smoothstep ramps, so the renderer dissolves between
// surfaces instead of drawing a hard seam. Cosmetic output, but pure algebra
// over the lockstep-safe up-dot + height (no acos), so it is bit-identical
// across platforms for the same serialized terrain.

#include "world/outdoor/Splatmap.h"
#include "world/outdoor/Terrain.h"

#include "math/Math.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cmath>
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

f32 weight_sum(const SplatWeights& w) {
    return w.grass + w.rock + w.snow + w.sand;
}
}  // namespace

TEST_CASE("splatmap: the four weights always sum to one",
          "[terrain][gameplay]") {
    // A mixed field: a +X ramp rising from sea level into the snow band so the
    // sampled points span sand / grass / rock / snow regimes.
    std::vector<u16> hm(8 * 8, 0u);
    for (u32 z = 0; z < 8; ++z)
        for (u32 x = 0; x < 8; ++x)
            hm[z * 8 + x] = static_cast<u16>(x * 1200u);  // 0..84 m (scale 0.01)
    const HeightmapDesc d = make_desc(hm, 8, 8, 1.0f, 0.01f);

    for (f32 wx = 0.5f; wx <= 6.5f; wx += 0.75f) {
        const SplatWeights w = terrain_splat(d, wx, 4.0f, kDefaultSplatBands);
        REQUIRE(weight_sum(w) == Catch::Approx(1.0f).margin(1e-5f));
        // Every component stays a valid weight.
        REQUIRE(w.grass >= 0.0f);
        REQUIRE(w.rock >= 0.0f);
        REQUIRE(w.snow >= 0.0f);
        REQUIRE(w.sand >= 0.0f);
        REQUIRE(w.grass <= 1.0f);
        REQUIRE(w.rock <= 1.0f);
        REQUIRE(w.snow <= 1.0f);
        REQUIRE(w.sand <= 1.0f);
    }
}

TEST_CASE("splatmap: flat mid-elevation ground is mostly grass",
          "[terrain][gameplay]") {
    // scale 0.01 -> u16 1000 == 10 m, flat (up-dot ~ 1, no rock). 10 m sits
    // above the sand fade (out by 4 m) and below the snow ramp (in from 50 m),
    // so grass dominates.
    std::vector<u16> hm(8 * 8, 1000u);
    const HeightmapDesc d = make_desc(hm, 8, 8, 1.0f, 0.01f);

    const SplatWeights w = terrain_splat(d, 4.0f, 4.0f, kDefaultSplatBands);
    REQUIRE(w.grass > 0.9f);
    REQUIRE(w.grass > w.rock);
    REQUIRE(w.grass > w.snow);
    REQUIRE(w.grass > w.sand);
    REQUIRE(weight_sum(w) == Catch::Approx(1.0f).margin(1e-5f));
}

TEST_CASE("splatmap: a steep slope is mostly rock", "[terrain][gameplay]") {
    // heights[x] = x*200 -> 2 m rise per 1 m run (scale 0.01, spacing 1 m): a
    // ~63deg ramp, up-dot ~ 0.447, at/below rock_slope_lo (0.574), so the rock
    // ramp is fully on and rock dominates the blend.
    std::vector<u16> hm(8 * 8, 0u);
    for (u32 z = 0; z < 8; ++z)
        for (u32 x = 0; x < 8; ++x) hm[z * 8 + x] = static_cast<u16>(x * 200u);
    const HeightmapDesc d = make_desc(hm, 8, 8, 1.0f, 0.01f);

    const SplatWeights w = terrain_splat(d, 4.0f, 4.0f, kDefaultSplatBands);
    REQUIRE(w.rock > 0.5f);
    REQUIRE(w.rock > w.grass);
    REQUIRE(w.rock > w.snow);
    REQUIRE(w.rock > w.sand);
    REQUIRE(weight_sum(w) == Catch::Approx(1.0f).margin(1e-5f));
}

TEST_CASE("splatmap: high flat ground is mostly snow", "[terrain][gameplay]") {
    // scale 0.01 -> u16 8000 == 80 m, flat. 80 m is above snow_height_hi (70 m)
    // and the ground is flat (no rock gate), so snow dominates.
    std::vector<u16> hm(8 * 8, 8000u);
    const HeightmapDesc d = make_desc(hm, 8, 8, 1.0f, 0.01f);

    const SplatWeights w = terrain_splat(d, 4.0f, 4.0f, kDefaultSplatBands);
    REQUIRE(w.snow > 0.9f);
    REQUIRE(w.snow > w.grass);
    REQUIRE(w.snow > w.rock);
    REQUIRE(w.snow > w.sand);
    REQUIRE(weight_sum(w) == Catch::Approx(1.0f).margin(1e-5f));
}

TEST_CASE("splatmap: very low flat ground has sand weight",
          "[terrain][gameplay]") {
    // scale 0.01 -> u16 50 == 0.5 m, flat. 0.5 m is below sand_lo
    // (sand_height_hi 4 - blend_range 4 = 0 m), so sand is fully on and
    // dominates.
    std::vector<u16> hm(8 * 8, 50u);
    const HeightmapDesc d = make_desc(hm, 8, 8, 1.0f, 0.01f);

    const SplatWeights w = terrain_splat(d, 4.0f, 4.0f, kDefaultSplatBands);
    REQUIRE(w.sand > 0.0f);
    REQUIRE(w.sand > w.snow);
    REQUIRE(w.sand > w.rock);
    REQUIRE(weight_sum(w) == Catch::Approx(1.0f).margin(1e-5f));
}

TEST_CASE("splatmap: a transition zone blends two materials",
          "[terrain][gameplay]") {
    // Flat ground sitting INSIDE the snow ramp (between snow_height_lo 50 m and
    // snow_height_hi 70 m) cross-fades grass <-> snow: both strictly in (0,1).
    // scale 0.01 -> u16 6000 == 60 m, the ramp midpoint.
    std::vector<u16> hm(8 * 8, 6000u);
    const HeightmapDesc d = make_desc(hm, 8, 8, 1.0f, 0.01f);

    const SplatWeights w = terrain_splat(d, 4.0f, 4.0f, kDefaultSplatBands);
    REQUIRE(w.snow > 0.0f);
    REQUIRE(w.snow < 1.0f);
    REQUIRE(w.grass > 0.0f);
    REQUIRE(w.grass < 1.0f);
    REQUIRE(weight_sum(w) == Catch::Approx(1.0f).margin(1e-5f));
}

TEST_CASE("splatmap: smoothstep01 is a clamped Hermite ramp",
          "[terrain][gameplay]") {
    // 0 at/below edge0, 1 at/above edge1, 0.5 exactly at the midpoint.
    REQUIRE(smoothstep01(0.0f, 1.0f, -0.5f) ==
            Catch::Approx(0.0f).margin(1e-6f));
    REQUIRE(smoothstep01(0.0f, 1.0f, 0.0f) ==
            Catch::Approx(0.0f).margin(1e-6f));
    REQUIRE(smoothstep01(0.0f, 1.0f, 1.0f) ==
            Catch::Approx(1.0f).margin(1e-6f));
    REQUIRE(smoothstep01(0.0f, 1.0f, 1.5f) ==
            Catch::Approx(1.0f).margin(1e-6f));
    REQUIRE(smoothstep01(0.0f, 1.0f, 0.5f) ==
            Catch::Approx(0.5f).margin(1e-6f));
    // Midpoint of an arbitrary band is still 0.5.
    REQUIRE(smoothstep01(2.0f, 6.0f, 4.0f) ==
            Catch::Approx(0.5f).margin(1e-6f));
    // Monotonic: a point further along is never lower.
    REQUIRE(smoothstep01(0.0f, 1.0f, 0.25f) <= smoothstep01(0.0f, 1.0f, 0.75f));
}

TEST_CASE("splatmap: smoothstep01 degenerates to a hard step when edges meet",
          "[terrain][gameplay]") {
    // edge0 == edge1 -> 0 strictly below, 1 at/above (no divide by zero).
    REQUIRE(smoothstep01(5.0f, 5.0f, 4.999f) ==
            Catch::Approx(0.0f).margin(1e-6f));
    REQUIRE(smoothstep01(5.0f, 5.0f, 5.0f) ==
            Catch::Approx(1.0f).margin(1e-6f));
    REQUIRE(smoothstep01(5.0f, 5.0f, 5.001f) ==
            Catch::Approx(1.0f).margin(1e-6f));
}

TEST_CASE("splatmap: weights are bit-identical across repeats",
          "[terrain][gameplay][determinism]") {
    std::vector<u16> hm(8 * 8, 0u);
    for (u32 z = 0; z < 8; ++z)
        for (u32 x = 0; x < 8; ++x) hm[z * 8 + x] = static_cast<u16>(x * 1200u);
    const HeightmapDesc d = make_desc(hm, 8, 8, 1.0f, 0.01f);

    const SplatWeights a = terrain_splat(d, 3.5f, 2.25f, kDefaultSplatBands);
    const SplatWeights b = terrain_splat(d, 3.5f, 2.25f, kDefaultSplatBands);
    // Exact equality: pure algebra + polynomial smoothstep, no transcendental.
    REQUIRE(a.grass == b.grass);
    REQUIRE(a.rock == b.rock);
    REQUIRE(a.snow == b.snow);
    REQUIRE(a.sand == b.sand);
}
