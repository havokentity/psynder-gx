// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/ai_avoidance.cpp — local agent-avoidance steering: with no
// neighbours avoid_velocity is the desired velocity clamped; a head-on threat
// bends the steered velocity away (lateral / opposing change) while staying
// within max_speed; a neighbour moving away leaves the desired velocity
// essentially unchanged; time_to_collision is small+positive for a head-on
// close and the large/negative sentinel otherwise; a dense cluster pushes harder
// than one far neighbour; and identical inputs are bit-deterministic.

#include "ai/Avoidance.h"

#include "math/Math.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <span>
#include <vector>

using namespace psynder;
using namespace psynder::ai;

namespace {

// XZ distance from desired_vel to result — how much avoidance bent the velocity.
f32 xz_deviation(math::Vec3 desired, math::Vec3 result) {
    const f32 dx = result.x - desired.x;
    const f32 dz = result.z - desired.z;
    return std::sqrt(dx * dx + dz * dz);
}

}  // namespace

TEST_CASE("ai: avoid with no neighbours returns the desired velocity clamped",
          "[ai][avoidance]") {
    const AvoidAgent self{{0.0f, 0.0f, 0.0f}, {3.0f, 0.0f, 0.0f}, 0.5f};
    const std::vector<AvoidAgent> none;

    const math::Vec3 desired{3.0f, 0.0f, 0.0f};
    const math::Vec3 out =
        avoid_velocity(self, desired, std::span<const AvoidAgent>(none),
                       2.0f, 5.0f);

    REQUIRE(out.x == Catch::Approx(3.0f));
    REQUIRE(out.z == Catch::Approx(0.0f));
    REQUIRE(out.y == 0.0f);
}

TEST_CASE("ai: avoid with no neighbours still clamps an over-fast desire",
          "[ai][avoidance]") {
    const AvoidAgent self{{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, 0.5f};
    const std::vector<AvoidAgent> none;

    // Desired (9,0,12) has magnitude 15; max_speed 5 -> capped to (3,0,4).
    const math::Vec3 desired{9.0f, 0.0f, 12.0f};
    const math::Vec3 out =
        avoid_velocity(self, desired, std::span<const AvoidAgent>(none),
                       2.0f, 5.0f);

    REQUIRE(math::length(out) == Catch::Approx(5.0f));
    REQUIRE(out.x == Catch::Approx(3.0f));
    REQUIRE(out.z == Catch::Approx(4.0f));
    REQUIRE(out.y == 0.0f);
}

TEST_CASE("ai: head-on threat bends the steered velocity and stays in budget",
          "[ai][avoidance]") {
    // Self heads +X; a neighbour 4 m ahead heads -X straight at it -> a close
    // head-on collision course.
    const f32 max_speed = 5.0f;
    const AvoidAgent self{{0.0f, 0.0f, 0.0f}, {max_speed, 0.0f, 0.0f}, 0.5f};
    const std::vector<AvoidAgent> nbrs{
        {{4.0f, 0.0f, 0.0f}, {-max_speed, 0.0f, 0.0f}, 0.5f}};

    const math::Vec3 desired{max_speed, 0.0f, 0.0f};
    const math::Vec3 out =
        avoid_velocity(self, desired, std::span<const AvoidAgent>(nbrs),
                       2.0f, max_speed);

    // The steered velocity must differ from the straight-ahead desire: either a
    // lateral (Z) veer or a reduced forward (X) component, and ideally both.
    REQUIRE(xz_deviation(desired, out) > 0.1f);
    REQUIRE(std::abs(out.z) > 0.1f);  // a real sideways veer appeared

    // Still inside the speed budget (clamp held).
    REQUIRE(math::length(out) <= Catch::Approx(max_speed).margin(1.0e-4f));
    REQUIRE(out.y == 0.0f);
}

TEST_CASE("ai: a neighbour moving away barely changes the desired velocity",
          "[ai][avoidance]") {
    // Self heads +X; the neighbour is ahead but fleeing faster in +X, so the gap
    // only grows -> no imminent collision -> the desire passes through clamped.
    const f32 max_speed = 5.0f;
    const AvoidAgent self{{0.0f, 0.0f, 0.0f}, {max_speed, 0.0f, 0.0f}, 0.5f};
    const std::vector<AvoidAgent> nbrs{
        {{4.0f, 0.0f, 0.0f}, {max_speed * 2.0f, 0.0f, 0.0f}, 0.5f}};

    const math::Vec3 desired{max_speed, 0.0f, 0.0f};
    const math::Vec3 out =
        avoid_velocity(self, desired, std::span<const AvoidAgent>(nbrs),
                       2.0f, max_speed);

    REQUIRE(out.x == Catch::Approx(max_speed));
    REQUIRE(out.z == Catch::Approx(0.0f));
    REQUIRE(out.y == 0.0f);
}

TEST_CASE("ai: time_to_collision is small and positive for a head-on close",
          "[ai][avoidance]") {
    // Two unit-radius discs 10 m apart on X closing at 1 m/s each. Centre gap
    // 10, combined radii 2, closing at 2 m/s -> first touch at (10-2)/2 == 4 s.
    const AvoidAgent a{{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, 1.0f};
    const AvoidAgent b{{10.0f, 0.0f, 0.0f}, {-1.0f, 0.0f, 0.0f}, 1.0f};

    const f32 t = time_to_collision(a, b);
    REQUIRE(t > 0.0f);
    REQUIRE(t == Catch::Approx(4.0f));
    REQUIRE(t < kNoCollision);
}

TEST_CASE("ai: time_to_collision is the sentinel for agents moving apart",
          "[ai][avoidance]") {
    // Same start, but each moving AWAY from the other -> never collide.
    const AvoidAgent a{{0.0f, 0.0f, 0.0f}, {-1.0f, 0.0f, 0.0f}, 1.0f};
    const AvoidAgent b{{10.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, 1.0f};

    const f32 t = time_to_collision(a, b);
    REQUIRE(t == Catch::Approx(kNoCollision));
}

TEST_CASE("ai: time_to_collision flags an existing overlap as negative",
          "[ai][avoidance]") {
    // Centres 1 m apart but combined radii 2 m -> already interpenetrating.
    const AvoidAgent a{{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, 1.0f};
    const AvoidAgent b{{1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, 1.0f};

    const f32 t = time_to_collision(a, b);
    REQUIRE(t < 0.0f);
}

TEST_CASE("ai: time_to_collision is the sentinel for a stationary clear pair",
          "[ai][avoidance]") {
    // Not overlapping and no relative motion -> never touch.
    const AvoidAgent a{{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, 1.0f};
    const AvoidAgent b{{10.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, 1.0f};

    const f32 t = time_to_collision(a, b);
    REQUIRE(t == Catch::Approx(kNoCollision));
}

TEST_CASE("ai: a dense cluster pushes harder than one far neighbour",
          "[ai][avoidance]") {
    const f32 max_speed = 5.0f;
    const AvoidAgent self{{0.0f, 0.0f, 0.0f}, {max_speed, 0.0f, 0.0f}, 0.5f};
    const math::Vec3 desired{max_speed, 0.0f, 0.0f};

    // A single neighbour right at the edge of the horizon (far, slow approach).
    const std::vector<AvoidAgent> single{
        {{9.0f, 0.0f, 0.0f}, {-1.0f, 0.0f, 0.0f}, 0.5f}};
    const math::Vec3 out_single =
        avoid_velocity(self, desired, std::span<const AvoidAgent>(single),
                       2.0f, max_speed);

    // A dense cluster of three near neighbours bearing down head-on.
    const std::vector<AvoidAgent> cluster{
        {{2.0f, 0.0f, 0.3f}, {-max_speed, 0.0f, 0.0f}, 0.5f},
        {{2.0f, 0.0f, 0.0f}, {-max_speed, 0.0f, 0.0f}, 0.5f},
        {{2.0f, 0.0f, -0.3f}, {-max_speed, 0.0f, 0.0f}, 0.5f}};
    const math::Vec3 out_cluster =
        avoid_velocity(self, desired, std::span<const AvoidAgent>(cluster),
                       2.0f, max_speed);

    REQUIRE(xz_deviation(desired, out_cluster) >
            xz_deviation(desired, out_single));

    // Both remain within the speed budget.
    REQUIRE(math::length(out_single) <= Catch::Approx(max_speed).margin(1.0e-4f));
    REQUIRE(math::length(out_cluster) <=
            Catch::Approx(max_speed).margin(1.0e-4f));
}

TEST_CASE("ai: avoidance is deterministic", "[ai][avoidance][determinism]") {
    const AvoidAgent self{{0.0f, 0.0f, 0.0f}, {4.0f, 0.0f, 1.0f}, 0.5f};
    const std::vector<AvoidAgent> nbrs{
        {{3.0f, 0.0f, 0.5f}, {-4.0f, 0.0f, 0.0f}, 0.5f},
        {{2.5f, 0.0f, -0.5f}, {-3.0f, 0.0f, 1.0f}, 0.6f},
        {{6.0f, 0.0f, 0.0f}, {2.0f, 0.0f, 0.0f}, 0.5f}};
    const math::Vec3 desired{4.0f, 0.0f, 1.0f};

    const auto run = [&]() {
        const math::Vec3 v =
            avoid_velocity(self, desired, std::span<const AvoidAgent>(nbrs),
                           2.0f, 6.0f);
        return std::vector<f32>{v.x, v.y, v.z};
    };

    REQUIRE(run() == run());

    // And time_to_collision itself is bit-stable.
    const f32 t0 = time_to_collision(self, nbrs[0]);
    const f32 t1 = time_to_collision(self, nbrs[0]);
    REQUIRE(t0 == t1);
}
