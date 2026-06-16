// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/terrain_visibility.cpp — deterministic terrain line-of-sight /
// visibility queries (engine/world/outdoor/TerrainVisibility): does the terrain
// occlude the straight segment between two world points? The tactical-AI query
// used to decide whether a bot can see / shoot a target over the hills on a
// Battlefield-light outdoor map. Built on HeightfieldQuery's bilinear height.

#include "world/outdoor/TerrainVisibility.h"
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

// A 16x16 map (spacing 1 m, scale 0.01 -> metres = units/100): a flat 2 m base
// everywhere with a TALL central hill that peaks ~30 m near x=8 and tapers back
// down to the flat base toward the +X / -X edges. Two low/flat regions flank
// the hill (around x=2 and x=13 the surface is at the 2 m base), so a low line
// strung between them must pass OVER the 30 m hill in the middle.
std::vector<u16> make_hill_field() {
    constexpr u32 N = 16u;
    constexpr i32 base = 200;    // 2 m base
    constexpr i32 peak = 3000;   // 30 m peak
    constexpr i32 cx = 8;        // hill centre column
    std::vector<u16> hm(N * N, 0u);
    for (u32 z = 0; z < N; ++z) {
        for (u32 x = 0; x < N; ++x) {
            // Triangular ridge along X: full peak at the centre column, falling
            // off linearly to the base over `cx` columns to either side. Uniform
            // in Z so the hill is a ridge crossing the whole map.
            const i32 d = (static_cast<i32>(x) - cx);
            const i32 dist = d < 0 ? -d : d;       // |x - cx|
            i32 h = peak - dist * (peak - base) / cx;
            if (h < base) h = base;
            hm[z * N + x] = static_cast<u16>(h);
        }
    }
    return hm;
}
}  // namespace

TEST_CASE("terrain-vis: line of sight across flat ground is clear", "[terrain][gameplay]") {
    std::vector<u16> hm(16 * 16, 200u);  // flat 2 m everywhere
    const HeightmapDesc d = make_desc(hm, 16, 16, 1.0f, 0.01f);

    // Two eyes at 4 m over flat 2 m ground: ~2 m of clearance the whole way.
    const math::Vec3 a{2.0f, 4.0f, 8.0f};
    const math::Vec3 b{13.0f, 4.0f, 8.0f};
    REQUIRE(terrain_line_of_sight(d, a, b));
    REQUIRE(terrain_los_clearance(d, a, b) == Catch::Approx(2.0f).margin(1e-4f));
    REQUIRE(terrain_los_clearance(d, a, b) > 0.0f);
}

TEST_CASE("terrain-vis: a tall hill occludes a low line", "[terrain][gameplay]") {
    const std::vector<u16> hm = make_hill_field();
    const HeightmapDesc d = make_desc(hm, 16, 16, 1.0f, 0.01f);

    // Two low points (~4 m) on opposite sides of the 30 m central ridge. The
    // straight segment stays low while the ground rears up to 30 m -> blocked.
    const math::Vec3 a{2.0f, 4.0f, 8.0f};
    const math::Vec3 b{13.0f, 4.0f, 8.0f};
    REQUIRE_FALSE(terrain_line_of_sight(d, a, b));
    REQUIRE(terrain_los_clearance(d, a, b) <= 0.0f);  // dips into the hill
    REQUIRE(terrain_los_clearance(d, a, b) < -10.0f); // by a large margin
}

TEST_CASE("terrain-vis: raising both endpoints over the hill restores sight",
          "[terrain][gameplay]") {
    const std::vector<u16> hm = make_hill_field();
    const HeightmapDesc d = make_desc(hm, 16, 16, 1.0f, 0.01f);

    // Same XZ endpoints, but both eyes lifted to 40 m — well above the 30 m
    // ridge — so the line clears the crest everywhere.
    const math::Vec3 a{2.0f, 40.0f, 8.0f};
    const math::Vec3 b{13.0f, 40.0f, 8.0f};
    REQUIRE(terrain_line_of_sight(d, a, b));
    REQUIRE(terrain_los_clearance(d, a, b) > 0.0f);
}

TEST_CASE("terrain-vis: a zero-length segment is visible", "[terrain][gameplay]") {
    const std::vector<u16> hm = make_hill_field();
    const HeightmapDesc d = make_desc(hm, 16, 16, 1.0f, 0.01f);

    // a == b: no interior to occlude, even sitting on top of the hill.
    const math::Vec3 a{8.0f, 5.0f, 8.0f};
    REQUIRE(terrain_line_of_sight(d, a, a));
    REQUIRE(terrain_los_clearance(d, a, a) > 0.0f);
}

TEST_CASE("terrain-vis: clearance_m margin fails a marginally-clear line",
          "[terrain][gameplay]") {
    std::vector<u16> hm(16 * 16, 200u);  // flat 2 m
    const HeightmapDesc d = make_desc(hm, 16, 16, 1.0f, 0.01f);

    // Eyes at 4 m over flat 2 m ground: actual clearance is ~2 m everywhere.
    const math::Vec3 a{2.0f, 4.0f, 8.0f};
    const math::Vec3 b{13.0f, 4.0f, 8.0f};
    const f32 clr = terrain_los_clearance(d, a, b);
    REQUIRE(clr == Catch::Approx(2.0f).margin(1e-4f));

    // A 1 m required margin is satisfied; a 3 m required margin is not.
    REQUIRE(terrain_line_of_sight(d, a, b, /*clearance_m=*/1.0f));
    REQUIRE_FALSE(terrain_line_of_sight(d, a, b, /*clearance_m=*/3.0f));
}

TEST_CASE("terrain-vis: clearance sign agrees with line-of-sight", "[terrain][gameplay]") {
    const std::vector<u16> hm = make_hill_field();
    const HeightmapDesc d = make_desc(hm, 16, 16, 1.0f, 0.01f);

    // Occluded low line: false <-> clearance <= 0.
    const math::Vec3 lo_a{2.0f, 4.0f, 8.0f};
    const math::Vec3 lo_b{13.0f, 4.0f, 8.0f};
    REQUIRE(terrain_line_of_sight(d, lo_a, lo_b) ==
            (terrain_los_clearance(d, lo_a, lo_b) >= 0.0f));

    // Clear high line: true <-> clearance > 0. The default-margin LoS is exactly
    // the `clearance >= 0` predicate.
    const math::Vec3 hi_a{2.0f, 40.0f, 8.0f};
    const math::Vec3 hi_b{13.0f, 40.0f, 8.0f};
    REQUIRE(terrain_line_of_sight(d, hi_a, hi_b) ==
            (terrain_los_clearance(d, hi_a, hi_b) >= 0.0f));

    // The margin overload is equivalent to comparing the clearance directly.
    const f32 c = terrain_los_clearance(d, hi_a, hi_b);
    REQUIRE(terrain_line_of_sight(d, hi_a, hi_b, c));            // >= c
    REQUIRE_FALSE(terrain_line_of_sight(d, hi_a, hi_b, c + 1.0f));  // > c fails
}

TEST_CASE("terrain-vis: repeated queries are bit-identical",
          "[terrain][gameplay][determinism]") {
    const std::vector<u16> hm = make_hill_field();
    const HeightmapDesc d = make_desc(hm, 16, 16, 1.0f, 0.01f);

    const math::Vec3 a{2.5f, 12.0f, 7.25f};
    const math::Vec3 b{13.5f, 9.0f, 8.75f};
    const f32 c0 = terrain_los_clearance(d, a, b);
    const f32 c1 = terrain_los_clearance(d, a, b);
    REQUIRE(c0 == c1);  // exact: pure algebra, no transcendental on the value path
    REQUIRE(terrain_line_of_sight(d, a, b) == terrain_line_of_sight(d, a, b));
}
