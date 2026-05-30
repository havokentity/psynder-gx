// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/ai_steering.cpp — Reynolds steering primitives: seek pulls toward
// the target at max_speed (zero on top of it), flee pushes the opposite way,
// arrive ramps speed down inside the slow radius, clamp_speed caps an over-fast
// velocity, blend mixes weighted, and the zero-length guards stay NaN-free and
// bit-deterministic.

#include "ai/Steering.h"

#include "math/Math.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

using namespace psynder;
using namespace psynder::ai;

TEST_CASE("ai: seek points toward the target at max speed", "[ai][steering]") {
    // Target is +X and +Z of the agent; desired velocity should point that way.
    const math::Vec3 v = seek({0.0f, 0.0f, 0.0f}, {3.0f, 0.0f, 4.0f}, 10.0f);
    REQUIRE(v.x > 0.0f);
    REQUIRE(v.z > 0.0f);
    REQUIRE(v.y == 0.0f);
    // Magnitude equals max_speed (3,4,5 triangle -> unit * 10 = (6,8)).
    REQUIRE(v.x == Catch::Approx(6.0f));
    REQUIRE(v.z == Catch::Approx(8.0f));
    REQUIRE(math::length(v) == Catch::Approx(10.0f));
}

TEST_CASE("ai: seek is zero exactly at the target", "[ai][steering]") {
    const math::Vec3 v = seek({2.0f, 5.0f, -1.0f}, {2.0f, 9.0f, -1.0f}, 7.0f);
    REQUIRE(v.x == 0.0f);
    REQUIRE(v.y == 0.0f);
    REQUIRE(v.z == 0.0f);
}

TEST_CASE("ai: seek ignores the y axis (XZ-plane convention)", "[ai][steering]") {
    // Same XZ as the target but a big y gap: still steers purely in XZ, y == 0.
    const math::Vec3 v = seek({0.0f, 0.0f, 0.0f}, {5.0f, 100.0f, 0.0f}, 4.0f);
    REQUIRE(v.x == Catch::Approx(4.0f));
    REQUIRE(v.z == Catch::Approx(0.0f));
    REQUIRE(v.y == 0.0f);
}

TEST_CASE("ai: flee points away from the threat", "[ai][steering]") {
    // Threat at origin, agent at +X: flee must drive further +X at max_speed.
    const math::Vec3 v = flee({2.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, 5.0f);
    REQUIRE(v.x > 0.0f);
    REQUIRE(v.z == Catch::Approx(0.0f));
    REQUIRE(v.y == 0.0f);
    REQUIRE(math::length(v) == Catch::Approx(5.0f));

    // flee is the exact opposite of seek toward the same point.
    const math::Vec3 s = seek({2.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, 5.0f);
    REQUIRE(v.x == Catch::Approx(-s.x));
    REQUIRE(v.z == Catch::Approx(-s.z));
}

TEST_CASE("ai: arrive runs full speed outside the slow radius", "[ai][steering]") {
    // 10 m from the target, slow radius 2 m -> full max_speed.
    const math::Vec3 v = arrive({0.0f, 0.0f, 0.0f}, {10.0f, 0.0f, 0.0f},
                                6.0f, 2.0f);
    REQUIRE(v.x == Catch::Approx(6.0f));
    REQUIRE(v.z == Catch::Approx(0.0f));
    REQUIRE(v.y == 0.0f);
    REQUIRE(math::length(v) == Catch::Approx(6.0f));
}

TEST_CASE("ai: arrive ramps speed down inside the slow radius", "[ai][steering]") {
    const f32 max_speed = 8.0f;
    const f32 slow = 4.0f;
    const math::Vec3 target{0.0f, 0.0f, 0.0f};

    // Half way in (2 m of a 4 m radius) -> half speed.
    const math::Vec3 mid = arrive({2.0f, 0.0f, 0.0f}, target, max_speed, slow);
    REQUIRE(math::length(mid) == Catch::Approx(4.0f));
    REQUIRE(mid.x < 0.0f);  // pointing back toward the target (-X)
    REQUIRE(mid.y == 0.0f);

    // Closer in (1 m) -> slower still than the mid sample.
    const math::Vec3 near = arrive({1.0f, 0.0f, 0.0f}, target, max_speed, slow);
    REQUIRE(math::length(near) == Catch::Approx(2.0f));
    REQUIRE(math::length(near) < math::length(mid));  // slower the closer it is
}

TEST_CASE("ai: arrive is zero at the target", "[ai][steering]") {
    const math::Vec3 v = arrive({3.0f, 1.0f, 3.0f}, {3.0f, 4.0f, 3.0f},
                                9.0f, 5.0f);
    REQUIRE(v.x == 0.0f);
    REQUIRE(v.y == 0.0f);
    REQUIRE(v.z == 0.0f);
}

TEST_CASE("ai: clamp_speed caps an over-fast velocity", "[ai][steering]") {
    // (9, 0, 12) has magnitude 15; cap to 5 -> (3, 0, 4).
    const math::Vec3 v = clamp_speed({9.0f, 0.0f, 12.0f}, 5.0f);
    REQUIRE(math::length(v) == Catch::Approx(5.0f));
    REQUIRE(v.x == Catch::Approx(3.0f));
    REQUIRE(v.z == Catch::Approx(4.0f));
    REQUIRE(v.y == 0.0f);
}

TEST_CASE("ai: clamp_speed leaves a slow velocity unchanged", "[ai][steering]") {
    const math::Vec3 v = clamp_speed({1.0f, 0.0f, 2.0f}, 10.0f);
    REQUIRE(v.x == Catch::Approx(1.0f));
    REQUIRE(v.z == Catch::Approx(2.0f));
    REQUIRE(v.y == 0.0f);
}

TEST_CASE("ai: blend sums two behaviors weighted", "[ai][steering]") {
    const math::Vec3 a{2.0f, 0.0f, 0.0f};
    const math::Vec3 b{0.0f, 0.0f, 4.0f};
    const math::Vec3 v = blend(a, b, 0.5f, 0.25f);
    REQUIRE(v.x == Catch::Approx(1.0f));   // 2 * 0.5
    REQUIRE(v.z == Catch::Approx(1.0f));   // 4 * 0.25
    REQUIRE(v.y == 0.0f);
}

TEST_CASE("ai: zero-length guards stay NaN-free", "[ai][steering]") {
    // seek/flee/arrive on a coincident point, and clamp_speed of a zero vector.
    const math::Vec3 s = seek({1.0f, 2.0f, 3.0f}, {1.0f, 9.0f, 3.0f}, 5.0f);
    const math::Vec3 f = flee({1.0f, 2.0f, 3.0f}, {1.0f, -7.0f, 3.0f}, 5.0f);
    const math::Vec3 a = arrive({1.0f, 0.0f, 3.0f}, {1.0f, 0.0f, 3.0f}, 5.0f, 2.0f);
    const math::Vec3 c = clamp_speed({0.0f, 0.0f, 0.0f}, 5.0f);
    for (const math::Vec3& v : std::vector<math::Vec3>{s, f, a, c}) {
        REQUIRE(v.x == 0.0f);
        REQUIRE(v.y == 0.0f);
        REQUIRE(v.z == 0.0f);
        REQUIRE_FALSE(std::isnan(v.x));
        REQUIRE_FALSE(std::isnan(v.z));
    }
}

TEST_CASE("ai: steering is deterministic", "[ai][steering][determinism]") {
    const auto run = []() {
        std::vector<f32> out;
        const math::Vec3 sk = seek({0.0f, 0.0f, 0.0f}, {3.0f, 0.0f, 4.0f}, 11.0f);
        const math::Vec3 fl = flee({3.0f, 0.0f, 4.0f}, {0.0f, 0.0f, 0.0f}, 11.0f);
        const math::Vec3 ar = arrive({1.0f, 0.0f, 1.0f}, {5.0f, 0.0f, 5.0f},
                                     9.0f, 3.0f);
        const math::Vec3 cl = clamp_speed({100.0f, 0.0f, 75.0f}, 6.0f);
        const math::Vec3 bl = blend(sk, fl, 0.7f, 0.3f);
        for (const math::Vec3& v : {sk, fl, ar, cl, bl}) {
            out.push_back(v.x);
            out.push_back(v.y);
            out.push_back(v.z);
        }
        return out;
    };
    REQUIRE(run() == run());
}
