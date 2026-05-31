// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/world_water.cpp — the water plane: submersion, buoyancy, drag.

#include "world/outdoor/Water.h"

#include "math/Math.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>  // std::abs (float)

using namespace psynder;
using namespace psynder::world::outdoor;

namespace {
constexpr f32 kG = 9.81f;
const WaterPlane kWater{10.0f};  // surface at y = 10 m
}  // namespace

TEST_CASE("water: underwater test and submersion depth", "[terrain]") {
    CHECK(is_underwater(kWater, math::Vec3{0.0f, 7.0f, 0.0f}));   // 3 m under
    CHECK_FALSE(is_underwater(kWater, math::Vec3{0.0f, 12.0f, 0.0f}));
    CHECK(submersion_depth(kWater, math::Vec3{0.0f, 7.0f, 0.0f}) == Catch::Approx(3.0f));
    CHECK(submersion_depth(kWater, math::Vec3{0.0f, 12.0f, 0.0f}) == Catch::Approx(0.0f));
    CHECK(submersion_depth(kWater, math::Vec3{0.0f, 10.0f, 0.0f}) == Catch::Approx(0.0f));
}

TEST_CASE("water: submersion fraction of a vertical body", "[terrain]") {
    // A 2 m tall body. Fully above (bottom at 11 -> top 13): 0.
    CHECK(submersion_fraction(kWater, 11.0f, 2.0f) == Catch::Approx(0.0f));
    // Fully submerged (bottom at 5 -> top 7, both under 10): 1.
    CHECK(submersion_fraction(kWater, 5.0f, 2.0f) == Catch::Approx(1.0f));
    // Half crossing the line (bottom 9 -> top 11, water at 10): 0.5.
    CHECK(submersion_fraction(kWater, 9.0f, 2.0f) == Catch::Approx(0.5f));
    // Degenerate body height: a point below the surface is fully submerged.
    CHECK(submersion_fraction(kWater, 8.0f, 0.0f) == Catch::Approx(1.0f));
    CHECK(submersion_fraction(kWater, 12.0f, -1.0f) == Catch::Approx(0.0f));
}

TEST_CASE("water: buoyancy scales with submersion and density ratio", "[terrain]") {
    // Out of the water: no buoyancy.
    CHECK(buoyancy_accel(0.0f, kG, 1.5f) == Catch::Approx(0.0f));
    // Fully submerged, float-ier body (ratio > 1) gets more than gravity up.
    const f32 full = buoyancy_accel(1.0f, kG, 1.5f);
    CHECK(full == Catch::Approx(kG * 1.5f));
    CHECK(full > kG);  // net upward (floats)
    // Half submerged is half the buoyancy.
    CHECK(buoyancy_accel(0.5f, kG, 1.5f) == Catch::Approx(full * 0.5f));
    // A denser-than-water body (ratio < 1) sinks: buoyancy < gravity when full.
    CHECK(buoyancy_accel(1.0f, kG, 0.8f) < kG);
    // Fraction clamps.
    CHECK(buoyancy_accel(2.0f, kG, 1.0f) == Catch::Approx(kG));
}

TEST_CASE("water: drag opposes velocity and grows with submersion and speed",
          "[terrain]") {
    // Moving up (+v) -> drag pushes down (-).
    CHECK(water_drag(1.0f, 5.0f, 0.5f) < 0.0f);
    // Moving down (-v) -> drag pushes up (+).
    CHECK(water_drag(1.0f, -5.0f, 0.5f) > 0.0f);
    // More submersion or more speed -> stronger drag magnitude.
    const f32 shallow = water_drag(0.25f, 5.0f, 0.5f);
    const f32 deep = water_drag(1.0f, 5.0f, 0.5f);
    CHECK(std::abs(deep) > std::abs(shallow));
    const f32 slow = water_drag(1.0f, 2.0f, 0.5f);
    const f32 fast = water_drag(1.0f, 8.0f, 0.5f);
    CHECK(std::abs(fast) > std::abs(slow));
    // Out of water or at rest -> no drag.
    CHECK(water_drag(0.0f, 5.0f, 0.5f) == Catch::Approx(0.0f));
    CHECK(water_drag(1.0f, 0.0f, 0.5f) == Catch::Approx(0.0f));
}

TEST_CASE("water: queries are deterministic", "[terrain][determinism]") {
    for (int i = 0; i < 16; ++i) {
        CHECK(submersion_fraction(kWater, 9.0f, 2.0f) ==
              submersion_fraction(kWater, 9.0f, 2.0f));
        CHECK(buoyancy_accel(0.5f, kG, 1.2f) == buoyancy_accel(0.5f, kG, 1.2f));
    }
}
