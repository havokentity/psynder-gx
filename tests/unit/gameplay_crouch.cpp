// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/gameplay_crouch.cpp — crouch state machine: smooth height easing,
// the can't-stand-up-under-a-ceiling rule, crouch fraction / speed multiplier,
// the dt guards, and determinism.

#include "gameplay/Crouch.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>

using namespace psynder;
using namespace psynder::gameplay;

namespace {
// A 1.8 m standing / 0.9 m crouched pawn that fully transitions in ~0.9 s at
// 1.0 m/s. Initialised standing.
Crouch make_pawn() {
    Crouch c{};
    c.stand_height_m = 1.8f;
    c.crouch_height_m = 0.9f;
    c.transition_rate_mps = 1.0f;  // 0.9 m span -> 0.9 s to fully ease
    crouch_init(c);
    return c;
}
}  // namespace

TEST_CASE("gameplay: a freshly init crouch stands at full height", "[gameplay]") {
    const Crouch c = make_pawn();
    REQUIRE(c.height_m == Catch::Approx(1.8f));
    REQUIRE(c.crouched == 0u);
    REQUIRE_FALSE(is_crouched(c));
    REQUIRE(crouch_fraction(c) == Catch::Approx(0.0f));
}

TEST_CASE("gameplay: holding crouch eases the height down to crouch height",
          "[gameplay]") {
    Crouch c = make_pawn();
    // One step of 0.5 s at 1 m/s lowers height by 0.5 m (1.8 -> 1.3), not yet down.
    crouch_update(c, /*want=*/true, /*blocked=*/false, 0.5f);
    REQUIRE(c.height_m == Catch::Approx(1.3f));
    REQUIRE(is_crouched(c));  // desired state latched immediately

    // Keep crouching: it converges on crouch_height_m and never overshoots below.
    for (int i = 0; i < 20; ++i)
        crouch_update(c, /*want=*/true, /*blocked=*/false, 0.1f);
    REQUIRE(c.height_m == Catch::Approx(0.9f));
    REQUIRE(c.height_m >= 0.9f);
    REQUIRE(is_crouched(c));
}

TEST_CASE("gameplay: releasing crouch eases back up to stand height when clear",
          "[gameplay]") {
    Crouch c = make_pawn();
    for (int i = 0; i < 20; ++i)
        crouch_update(c, /*want=*/true, /*blocked=*/false, 0.1f);
    REQUIRE(c.height_m == Catch::Approx(0.9f));

    // Release with headroom: rises back to standing, clamped (no overshoot above).
    for (int i = 0; i < 20; ++i)
        crouch_update(c, /*want=*/false, /*blocked=*/false, 0.1f);
    REQUIRE(c.height_m == Catch::Approx(1.8f));
    REQUIRE(c.height_m <= 1.8f);
    REQUIRE_FALSE(is_crouched(c));
}

TEST_CASE("gameplay: a ceiling overhead keeps the pawn crouched when crouch is released",
          "[gameplay]") {
    Crouch c = make_pawn();
    for (int i = 0; i < 20; ++i)
        crouch_update(c, /*want=*/true, /*blocked=*/false, 0.1f);
    REQUIRE(is_crouched(c));
    REQUIRE(c.height_m == Catch::Approx(0.9f));

    // Crouch released but something is overhead: still treated as crouched, and
    // the height stays down — cannot stand under a ceiling.
    for (int i = 0; i < 20; ++i)
        crouch_update(c, /*want=*/false, /*blocked=*/true, 0.1f);
    REQUIRE(is_crouched(c));
    REQUIRE(c.height_m == Catch::Approx(0.9f));

    // Once the ceiling clears, the released crouch finally stands up.
    for (int i = 0; i < 20; ++i)
        crouch_update(c, /*want=*/false, /*blocked=*/false, 0.1f);
    REQUIRE_FALSE(is_crouched(c));
    REQUIRE(c.height_m == Catch::Approx(1.8f));
}

TEST_CASE("gameplay: crouch fraction is 0 standing, 1 crouched, and mid in transition",
          "[gameplay]") {
    Crouch c = make_pawn();
    REQUIRE(crouch_fraction(c) == Catch::Approx(0.0f));  // standing

    // Halfway down (1.35 m of a 1.8 -> 0.9 span) is fraction 0.5.
    c.height_m = 1.35f;
    REQUIRE(crouch_fraction(c) == Catch::Approx(0.5f));

    // Fully crouched is fraction 1.
    c.height_m = 0.9f;
    REQUIRE(crouch_fraction(c) == Catch::Approx(1.0f));

    // Guard: equal stand/crouch heights -> fraction 0 (no /0).
    Crouch flat{};
    flat.stand_height_m = 1.5f;
    flat.crouch_height_m = 1.5f;
    flat.height_m = 1.5f;
    REQUIRE(crouch_fraction(flat) == Catch::Approx(0.0f));
}

TEST_CASE("gameplay: crouch speed multiplier interpolates between stand and crouch",
          "[gameplay]") {
    Crouch c = make_pawn();
    // Standing -> full stand multiplier.
    REQUIRE(crouch_speed_mult(c, 1.0f, 0.4f) == Catch::Approx(1.0f));

    // Fully crouched -> the crouch multiplier.
    c.height_m = 0.9f;
    REQUIRE(crouch_speed_mult(c, 1.0f, 0.4f) == Catch::Approx(0.4f));

    // Halfway -> the midpoint of the two multipliers.
    c.height_m = 1.35f;  // fraction 0.5
    REQUIRE(crouch_speed_mult(c, 1.0f, 0.4f) == Catch::Approx(0.7f));
}

TEST_CASE("gameplay: crouch update ignores zero and non-finite dt", "[gameplay]") {
    Crouch c = make_pawn();
    const f32 inf = std::numeric_limits<f32>::infinity();
    const f32 nan = std::numeric_limits<f32>::quiet_NaN();

    crouch_update(c, /*want=*/true, /*blocked=*/false, 0.0f);
    REQUIRE(c.height_m == Catch::Approx(1.8f));  // no movement, no latch
    REQUIRE_FALSE(is_crouched(c));

    crouch_update(c, /*want=*/true, /*blocked=*/false, -0.1f);
    REQUIRE(c.height_m == Catch::Approx(1.8f));
    REQUIRE_FALSE(is_crouched(c));

    crouch_update(c, /*want=*/true, /*blocked=*/false, inf);
    REQUIRE(c.height_m == Catch::Approx(1.8f));
    REQUIRE_FALSE(is_crouched(c));

    crouch_update(c, /*want=*/true, /*blocked=*/false, nan);
    REQUIRE(c.height_m == Catch::Approx(1.8f));
    REQUIRE_FALSE(is_crouched(c));
}

TEST_CASE("gameplay: crouch easing is deterministic across runs", "[gameplay]") {
    const auto run = []() {
        Crouch c = make_pawn();
        const f32 dt = 1.0f / 120.0f;
        // Crouch for a while, hold under a ceiling, then release and stand.
        for (int i = 0; i < 60; ++i)
            crouch_update(c, /*want=*/true, /*blocked=*/false, dt);
        for (int i = 0; i < 60; ++i)
            crouch_update(c, /*want=*/false, /*blocked=*/true, dt);
        for (int i = 0; i < 200; ++i)
            crouch_update(c, /*want=*/false, /*blocked=*/false, dt);
        return c.height_m;
    };
    REQUIRE(run() == run());  // bit-identical, no Approx
}
