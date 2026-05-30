// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/camera_fov_kick.cpp — dynamic FOV easing (sprint/ADS kick).

#include "camera/FovKick.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <limits>

using namespace psynder;
using namespace psynder::camera;

namespace {
FovParams make_params() {
    FovParams p;
    p.base_deg = 60.0f;
    p.sprint_deg = 75.0f;
    p.ads_deg = 45.0f;
    p.lerp_rate_per_s = 10.0f;
    return p;
}
}  // namespace

TEST_CASE("fov: target priority is aiming over sprinting over resting", "[camera]") {
    const FovParams p = make_params();
    CHECK(fov_target(p, false, false) == Catch::Approx(60.0f));  // rest
    CHECK(fov_target(p, true, false) == Catch::Approx(75.0f));   // sprint
    CHECK(fov_target(p, false, true) == Catch::Approx(45.0f));   // ADS
    CHECK(fov_target(p, true, true) == Catch::Approx(45.0f));    // ADS wins
}

TEST_CASE("fov: init sets the resting FOV", "[camera]") {
    const FovParams p = make_params();
    FovState s;
    s.current_deg = 123.0f;
    fov_init(s, p);
    CHECK(fov_value(s) == Catch::Approx(60.0f));
}

TEST_CASE("fov: update eases toward the target and converges", "[camera]") {
    const FovParams p = make_params();
    FovState s;
    fov_init(s, p);  // 60
    // Sprint: target 75. Each step moves part-way, never past it.
    f32 prev = fov_value(s);
    for (int i = 0; i < 200; ++i) {
        fov_update(s, p, /*sprint*/ true, /*aim*/ false, 1.0f / 60.0f);
        CHECK(fov_value(s) >= prev - 1e-4f);   // monotonic toward 75
        CHECK(fov_value(s) <= 75.0f + 1e-4f);  // never overshoots
        prev = fov_value(s);
    }
    CHECK(fov_value(s) == Catch::Approx(75.0f).margin(0.01f));  // converged
}

TEST_CASE("fov: a single step never passes the target", "[camera]") {
    FovParams p = make_params();
    p.lerp_rate_per_s = 1000.0f;  // huge rate
    FovState s;
    fov_init(s, p);  // 60
    // t clamps to 1, so one big step lands exactly on the target, not beyond.
    fov_update(s, p, true, false, 1.0f);  // rate*dt = 1000 -> clamped to 1
    CHECK(fov_value(s) == Catch::Approx(75.0f));
}

TEST_CASE("fov: switching target mid-ease redirects smoothly", "[camera]") {
    const FovParams p = make_params();
    FovState s;
    fov_init(s, p);
    for (int i = 0; i < 10; ++i) fov_update(s, p, true, false, 1.0f / 60.0f);
    const f32 mid = fov_value(s);
    CHECK(mid > 60.0f);
    CHECK(mid < 75.0f);
    // Now aim: target snaps to 45 and it eases downward past base.
    for (int i = 0; i < 300; ++i) fov_update(s, p, false, true, 1.0f / 60.0f);
    CHECK(fov_value(s) == Catch::Approx(45.0f).margin(0.01f));
}

TEST_CASE("fov: a non-finite or zero dt leaves the FOV untouched", "[camera]") {
    const FovParams p = make_params();
    FovState s;
    fov_init(s, p);
    const f32 before = fov_value(s);
    fov_update(s, p, true, false, 0.0f);  // dt 0 -> no move
    CHECK(fov_value(s) == before);
    fov_update(s, p, true, false, -1.0f);  // negative dt -> no move
    CHECK(fov_value(s) == before);
    const f32 inf = std::numeric_limits<f32>::infinity();
    fov_update(s, p, true, false, inf);  // non-finite -> no move
    CHECK(fov_value(s) == before);
}

TEST_CASE("fov: larger lerp rate converges faster", "[camera]") {
    const FovParams slow_p = [] { FovParams p = make_params(); p.lerp_rate_per_s = 2.0f; return p; }();
    const FovParams fast_p = [] { FovParams p = make_params(); p.lerp_rate_per_s = 20.0f; return p; }();
    FovState slow; fov_init(slow, slow_p);
    FovState fast; fov_init(fast, fast_p);
    for (int i = 0; i < 5; ++i) {
        fov_update(slow, slow_p, true, false, 1.0f / 60.0f);
        fov_update(fast, fast_p, true, false, 1.0f / 60.0f);
    }
    // Both heading to 75; the faster rate is closer.
    CHECK(fov_value(fast) > fov_value(slow));
}

TEST_CASE("fov: update is deterministic", "[camera][determinism]") {
    const FovParams p = make_params();
    FovState a; fov_init(a, p);
    FovState b; fov_init(b, p);
    for (int i = 0; i < 50; ++i) {
        const bool spr = (i % 3) == 0;
        const bool aim = (i % 5) == 0;
        fov_update(a, p, spr, aim, 1.0f / 90.0f);
        fov_update(b, p, spr, aim, 1.0f / 90.0f);
    }
    CHECK(fov_value(a) == fov_value(b));
}
