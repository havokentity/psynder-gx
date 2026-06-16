// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/ai_perception.cpp — FOV cone + range perception.

#include "ai/Perception.h"

#include "math/Math.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>

using namespace psynder;
using namespace psynder::ai;

namespace {
// Observer at origin facing +X, a 90 deg full cone (45 deg half-angle).
constexpr math::Vec3 kObs{0.0f, 0.0f, 0.0f};
constexpr math::Vec3 kFwd{1.0f, 0.0f, 0.0f};
const f32 kCos45 = fov_cos(45.0f);   // ~0.707
constexpr f32 kRange = 20.0f;
}  // namespace

TEST_CASE("perception: a target dead ahead in range and cone is perceived",
          "[ai]") {
    const math::Vec3 t{10.0f, 0.0f, 0.0f};  // straight ahead, half the range
    CHECK(can_perceive(kObs, kFwd, t, kCos45, kRange));
    CHECK(perception_strength(kObs, kFwd, t, kCos45, kRange) > 0.4f);  // strong
}

TEST_CASE("perception: a target behind the observer is not perceived", "[ai]") {
    const math::Vec3 behind{-10.0f, 0.0f, 0.0f};
    CHECK_FALSE(can_perceive(kObs, kFwd, behind, kCos45, kRange));
    CHECK(perception_strength(kObs, kFwd, behind, kCos45, kRange) == Catch::Approx(0.0f));
}

TEST_CASE("perception: a target beyond range is not perceived", "[ai]") {
    const math::Vec3 far_ahead{30.0f, 0.0f, 0.0f};  // > range
    CHECK_FALSE(can_perceive(kObs, kFwd, far_ahead, kCos45, kRange));
    CHECK(perception_strength(kObs, kFwd, far_ahead, kCos45, kRange) == Catch::Approx(0.0f));
}

TEST_CASE("perception: strength falls off with distance and angle", "[ai]") {
    const math::Vec3 near_c{5.0f, 0.0f, 0.0f};
    const math::Vec3 far_c{15.0f, 0.0f, 0.0f};
    CHECK(perception_strength(kObs, kFwd, near_c, kCos45, kRange) >
          perception_strength(kObs, kFwd, far_c, kCos45, kRange));

    // Same distance, one centered and one off-axis (still inside the 45 deg cone).
    const math::Vec3 centered{10.0f, 0.0f, 0.0f};
    const math::Vec3 off_axis{10.0f, 0.0f, 5.0f};  // ~26.5 deg off, in cone
    REQUIRE(can_perceive(kObs, kFwd, off_axis, kCos45, kRange));
    CHECK(perception_strength(kObs, kFwd, centered, kCos45, kRange) >
          perception_strength(kObs, kFwd, off_axis, kCos45, kRange));
}

TEST_CASE("perception: can_perceive agrees with strength for interior targets",
          "[ai]") {
    const std::array<math::Vec3, 3> pts{
        math::Vec3{8.0f, 0.0f, 2.0f}, math::Vec3{-3.0f, 0.0f, 0.0f},
        math::Vec3{12.0f, 0.0f, -4.0f}};
    for (const math::Vec3& p : pts) {
        const bool see = can_perceive(kObs, kFwd, p, kCos45, kRange);
        const f32 s = perception_strength(kObs, kFwd, p, kCos45, kRange);
        CHECK(see == (s > 0.0f));
    }
}

TEST_CASE("perception: a target on the observer is perceived", "[ai]") {
    CHECK(can_perceive(kObs, kFwd, kObs, kCos45, kRange));
    CHECK(perception_strength(kObs, kFwd, kObs, kCos45, kRange) == Catch::Approx(1.0f));
}

TEST_CASE("perception: fov_cos converts degrees to a cosine", "[ai]") {
    CHECK(fov_cos(60.0f) == Catch::Approx(0.5f));
    CHECK(fov_cos(0.0f) == Catch::Approx(1.0f));
    CHECK(fov_cos(90.0f) == Catch::Approx(0.0f).margin(1e-6f));
}

TEST_CASE("perception: most_perceptible picks the strongest and -1 for none",
          "[ai]") {
    const std::array<math::Vec3, 3> targets{
        math::Vec3{15.0f, 0.0f, 6.0f},   // far + off-axis
        math::Vec3{4.0f, 0.0f, 0.0f},    // near + centered (strongest)
        math::Vec3{12.0f, 0.0f, 0.0f}};  // mid + centered
    CHECK(most_perceptible(kObs, kFwd, targets, kCos45, kRange) == 1);

    const std::array<math::Vec3, 2> none{
        math::Vec3{-5.0f, 0.0f, 0.0f}, math::Vec3{100.0f, 0.0f, 0.0f}};
    CHECK(most_perceptible(kObs, kFwd, none, kCos45, kRange) == -1);
}

TEST_CASE("perception: queries are deterministic", "[ai][determinism]") {
    const math::Vec3 t{9.0f, 1.0f, 3.0f};
    for (int i = 0; i < 16; ++i) {
        CHECK(perception_strength(kObs, kFwd, t, kCos45, kRange) ==
              perception_strength(kObs, kFwd, t, kCos45, kRange));
    }
}
