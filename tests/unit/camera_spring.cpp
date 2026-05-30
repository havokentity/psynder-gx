// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/camera_spring.cpp — critically-damped smoothing spring (SmoothDamp).

#include "camera/Spring.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>   // std::isfinite (libstdc++ needs the explicit include)
#include <limits>

using namespace psynder;
using namespace psynder::camera;

TEST_CASE("spring: init sets the value and zeroes the velocity", "[camera]") {
    SpringState s;
    s.velocity = 7.0f;  // stale momentum that init must clear
    spring_init(s, 3.5f);
    CHECK(s.value == Catch::Approx(3.5f));
    CHECK(s.velocity == Catch::Approx(0.0f));
}

TEST_CASE("spring: update moves the value toward the target", "[camera]") {
    SpringState s;
    spring_init(s, 0.0f);
    const f32 returned = spring_update(s, 10.0f, 0.25f, 1.0f / 60.0f);
    // One step moves part-way toward the target (between start and target).
    CHECK(s.value > 0.0f);
    CHECK(s.value < 10.0f);
    CHECK(returned == Catch::Approx(s.value));  // returns the new value
}

TEST_CASE("spring: update converges to the target with zero velocity", "[camera]") {
    SpringState s;
    spring_init(s, 0.0f);
    for (int i = 0; i < 2000; ++i) {
        spring_update(s, 10.0f, 0.25f, 1.0f / 60.0f);
    }
    CHECK(s.value == Catch::Approx(10.0f).margin(1e-3f));
    CHECK(s.velocity == Catch::Approx(0.0f).margin(1e-3f));
}

TEST_CASE("spring: chasing upward does not wildly overshoot", "[camera]") {
    // Critically damped: chasing 10 from 0 must never sail past the target by
    // more than a sub-epsilon numerical wisp on any step.
    SpringState s;
    spring_init(s, 0.0f);
    const f32 target = 10.0f;
    const f32 overshoot_bound = target + 1e-3f;
    for (int i = 0; i < 2000; ++i) {
        const f32 v = spring_update(s, target, 0.25f, 1.0f / 60.0f);
        CHECK(v <= overshoot_bound);   // never meaningfully past the target
        CHECK(v >= 0.0f);              // and never dips below the start
    }
}

TEST_CASE("spring: a shorter smooth time converges faster", "[camera]") {
    SpringState fast;
    SpringState slow;
    spring_init(fast, 0.0f);
    spring_init(slow, 0.0f);
    // Step both toward the same target for the same wall time; the shorter
    // smooth_time must be closer to the target.
    for (int i = 0; i < 30; ++i) {
        spring_update(fast, 10.0f, 0.10f, 1.0f / 60.0f);
        spring_update(slow, 10.0f, 0.50f, 1.0f / 60.0f);
    }
    CHECK(fast.value > slow.value);                 // snappier got further
    CHECK((10.0f - fast.value) < (10.0f - slow.value));
}

TEST_CASE("spring: settled is false while chasing and true once close", "[camera]") {
    SpringState s;
    spring_init(s, 0.0f);
    // Far from the target and at rest velocity-wise, but the value gap fails.
    CHECK_FALSE(spring_settled(s, 10.0f, 1e-3f));
    for (int i = 0; i < 2000; ++i) {
        spring_update(s, 10.0f, 0.25f, 1.0f / 60.0f);
    }
    CHECK(spring_settled(s, 10.0f, 1e-2f));  // converged => settled
}

TEST_CASE("spring: settled needs both small distance and small velocity", "[camera]") {
    // On the target in value but still moving fast => NOT settled.
    SpringState s;
    s.value = 10.0f;
    s.velocity = 5.0f;
    CHECK_FALSE(spring_settled(s, 10.0f, 1e-2f));
    // On the target and at rest => settled.
    s.velocity = 0.0f;
    CHECK(spring_settled(s, 10.0f, 1e-2f));
}

TEST_CASE("spring: a zero or non-finite dt leaves the spring untouched", "[camera]") {
    SpringState s;
    spring_init(s, 2.0f);
    // Take a few real steps so there's a non-trivial value + velocity to guard.
    for (int i = 0; i < 5; ++i) spring_update(s, 10.0f, 0.25f, 1.0f / 60.0f);
    const f32 before_value = s.value;
    const f32 before_velocity = s.velocity;

    CHECK(spring_update(s, 10.0f, 0.25f, 0.0f) == before_value);   // dt 0
    CHECK(s.value == before_value);
    CHECK(s.velocity == before_velocity);

    CHECK(spring_update(s, 10.0f, 0.25f, -1.0f) == before_value);  // negative dt
    CHECK(s.value == before_value);
    CHECK(s.velocity == before_velocity);

    const f32 inf = std::numeric_limits<f32>::infinity();
    CHECK(spring_update(s, 10.0f, 0.25f, inf) == before_value);    // non-finite dt
    CHECK(s.value == before_value);
    CHECK(s.velocity == before_velocity);
}

TEST_CASE("spring: a non-finite target leaves the spring untouched", "[camera]") {
    SpringState s;
    spring_init(s, 2.0f);
    for (int i = 0; i < 5; ++i) spring_update(s, 10.0f, 0.25f, 1.0f / 60.0f);
    const f32 before_value = s.value;
    const f32 before_velocity = s.velocity;

    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    CHECK(spring_update(s, nan, 0.25f, 1.0f / 60.0f) == before_value);
    CHECK(s.value == before_value);
    CHECK(s.velocity == before_velocity);
}

TEST_CASE("spring: a tiny or non-finite smooth time stays finite", "[camera]") {
    // The smooth_time floor must keep omega finite (no divide-by-zero, no NaN).
    SpringState s;
    spring_init(s, 0.0f);
    const f32 v0 = spring_update(s, 10.0f, 0.0f, 1.0f / 60.0f);  // zero smooth_time
    CHECK(std::isfinite(v0));
    CHECK(std::isfinite(s.velocity));

    SpringState t;
    spring_init(t, 0.0f);
    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    const f32 v1 = spring_update(t, 10.0f, nan, 1.0f / 60.0f);   // non-finite st
    CHECK(std::isfinite(v1));
    CHECK(std::isfinite(t.velocity));
}

TEST_CASE("spring: update is deterministic", "[camera][determinism]") {
    SpringState a;
    SpringState b;
    spring_init(a, 0.0f);
    spring_init(b, 0.0f);
    // Vary the target across the sequence; identical inputs => identical state.
    for (int i = 0; i < 120; ++i) {
        const f32 target = (i % 2 == 0) ? 10.0f : -4.0f;
        spring_update(a, target, 0.2f, 1.0f / 90.0f);
        spring_update(b, target, 0.2f, 1.0f / 90.0f);
    }
    CHECK(a.value == b.value);        // bit-identical value
    CHECK(a.velocity == b.velocity);  // bit-identical velocity
}
