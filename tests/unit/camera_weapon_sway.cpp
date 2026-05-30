// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/camera_weapon_sway.cpp — weapon-sway viewmodel modifier (the gun
// lags the look, then recenters).

#include "camera/WeaponSway.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>

using namespace psynder;
using namespace psynder::camera;

namespace {
SwayParams make_params() {
    SwayParams p;
    p.sway_scale = 0.01f;
    p.max_offset = 0.05f;
    p.recenter_rate_per_s = 8.0f;
    return p;
}
}  // namespace

TEST_CASE("sway: init zeroes the offset", "[camera]") {
    SwayState s;
    s.offset_x = 1.0f;
    s.offset_y = -2.0f;
    sway_init(s);
    CHECK(s.offset_x == Catch::Approx(0.0f));
    CHECK(s.offset_y == Catch::Approx(0.0f));
}

TEST_CASE("sway: a fast look right pushes offset_x to the opposite sign", "[camera]") {
    const SwayParams p = make_params();
    SwayState s;
    sway_init(s);
    // Look to the RIGHT (positive yaw delta). The gun LAGS, so offset_x goes
    // NEGATIVE (it trails toward the left of screen as the view sweeps right).
    for (int i = 0; i < 30; ++i) {
        sway_update(s, p, /*yaw*/ 2.0f, /*pitch*/ 0.0f, 1.0f / 60.0f);
    }
    CHECK(s.offset_x < 0.0f);
    CHECK(s.offset_y == Catch::Approx(0.0f));

    // Look to the LEFT (negative yaw delta) mirrors: offset_x goes positive.
    SwayState s2;
    sway_init(s2);
    for (int i = 0; i < 30; ++i) {
        sway_update(s2, p, /*yaw*/ -2.0f, /*pitch*/ 0.0f, 1.0f / 60.0f);
    }
    CHECK(s2.offset_x > 0.0f);
}

TEST_CASE("sway: a huge look delta clamps the offset to max_offset", "[camera]") {
    const SwayParams p = make_params();
    SwayState s;
    sway_init(s);
    // An enormous look delta would push past the clamp; drive it many frames so
    // the offset fully converges to the clamped target, then assert the bound.
    for (int i = 0; i < 200; ++i) {
        sway_update(s, p, /*yaw*/ 100000.0f, /*pitch*/ 0.0f, 1.0f / 60.0f);
    }
    // Lag => negative; magnitude pinned to max_offset.
    CHECK(s.offset_x == Catch::Approx(-p.max_offset).margin(1e-6f));
    CHECK(std::abs(s.offset_x) <= p.max_offset + 1e-6f);
}

TEST_CASE("sway: with no look input the offset eases back to centre", "[camera]") {
    const SwayParams p = make_params();
    SwayState s;
    sway_init(s);
    // Push the gun out with a sustained look.
    for (int i = 0; i < 30; ++i) {
        sway_update(s, p, /*yaw*/ 3.0f, /*pitch*/ 0.0f, 1.0f / 60.0f);
    }
    CHECK(s.offset_x != Catch::Approx(0.0f));
    const f32 pushed = std::abs(s.offset_x);

    // Stop looking (deltas 0): the offset recenters toward 0.
    for (int i = 0; i < 300; ++i) {
        sway_update(s, p, /*yaw*/ 0.0f, /*pitch*/ 0.0f, 1.0f / 60.0f);
    }
    CHECK(std::abs(s.offset_x) < pushed);                  // moved back toward 0
    CHECK(s.offset_x == Catch::Approx(0.0f).margin(1e-4f)); // converged to centre
}

TEST_CASE("sway: offset_y responds to pitch and lags it", "[camera]") {
    const SwayParams p = make_params();
    SwayState s;
    sway_init(s);
    // Look UP (positive pitch delta). The gun LAGS, so offset_y goes NEGATIVE.
    for (int i = 0; i < 30; ++i) {
        sway_update(s, p, /*yaw*/ 0.0f, /*pitch*/ 2.0f, 1.0f / 60.0f);
    }
    CHECK(s.offset_y < 0.0f);
    CHECK(s.offset_x == Catch::Approx(0.0f));

    // Look DOWN mirrors: offset_y positive.
    SwayState s2;
    sway_init(s2);
    for (int i = 0; i < 30; ++i) {
        sway_update(s2, p, /*yaw*/ 0.0f, /*pitch*/ -2.0f, 1.0f / 60.0f);
    }
    CHECK(s2.offset_y > 0.0f);
}

TEST_CASE("sway: a non-finite look delta is treated as zero", "[camera]") {
    const SwayParams p = make_params();
    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    const f32 inf = std::numeric_limits<f32>::infinity();

    SwayState s;
    sway_init(s);
    // NaN yaw + Inf pitch must not poison the offset; both stay finite and at 0
    // (a non-finite delta is treated as no push -> target 0 -> stays centred).
    sway_update(s, p, nan, inf, 1.0f / 60.0f);
    CHECK(std::isfinite(s.offset_x));
    CHECK(std::isfinite(s.offset_y));
    CHECK(s.offset_x == Catch::Approx(0.0f));
    CHECK(s.offset_y == Catch::Approx(0.0f));

    // A non-finite delta arriving after a real push must not corrupt the carried
    // offset either — it just leaves the offset to ease toward 0.
    SwayState s2;
    sway_init(s2);
    for (int i = 0; i < 10; ++i) {
        sway_update(s2, p, 2.0f, 0.0f, 1.0f / 60.0f);
    }
    const f32 before = s2.offset_x;
    sway_update(s2, p, nan, nan, 1.0f / 60.0f);
    CHECK(std::isfinite(s2.offset_x));
    CHECK(std::isfinite(s2.offset_y));
    // Eased toward 0 (target 0 for a non-finite delta) -> magnitude shrank.
    CHECK(std::abs(s2.offset_x) <= std::abs(before) + 1e-6f);
}

TEST_CASE("sway: a non-finite or zero dt leaves the sway untouched", "[camera]") {
    const SwayParams p = make_params();
    SwayState s;
    sway_init(s);
    // Push the gun part-way so there's a non-trivial value to protect.
    for (int i = 0; i < 10; ++i) {
        sway_update(s, p, 2.0f, 1.0f, 1.0f / 60.0f);
    }
    const f32 bx = s.offset_x;
    const f32 by = s.offset_y;

    sway_update(s, p, 2.0f, 1.0f, 0.0f);   // dt 0 -> no move
    CHECK(s.offset_x == bx);
    CHECK(s.offset_y == by);

    sway_update(s, p, 2.0f, 1.0f, -1.0f);  // negative dt -> no move
    CHECK(s.offset_x == bx);
    CHECK(s.offset_y == by);

    const f32 inf = std::numeric_limits<f32>::infinity();
    sway_update(s, p, 2.0f, 1.0f, inf);    // non-finite dt -> no move
    CHECK(s.offset_x == bx);
    CHECK(s.offset_y == by);

    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    sway_update(s, p, 2.0f, 1.0f, nan);    // non-finite dt -> no move
    CHECK(s.offset_x == bx);
    CHECK(s.offset_y == by);
}

TEST_CASE("sway: a single big step lands exactly on the clamped target", "[camera]") {
    SwayParams p = make_params();
    p.recenter_rate_per_s = 1000.0f;  // rate*dt huge -> clamps to 1 -> snap
    SwayState s;
    sway_init(s);
    // Look right: target_x = clamp(-1.0 * 0.01) = -0.01; one step lands on it.
    sway_update(s, p, /*yaw*/ 1.0f, /*pitch*/ 0.0f, 1.0f);
    CHECK(s.offset_x == Catch::Approx(-0.01f));
    CHECK(s.offset_y == Catch::Approx(0.0f));
}

TEST_CASE("sway: reset zeroes the offset", "[camera]") {
    const SwayParams p = make_params();
    SwayState s;
    sway_init(s);
    for (int i = 0; i < 20; ++i) {
        sway_update(s, p, 3.0f, -2.0f, 1.0f / 60.0f);
    }
    CHECK(s.offset_x != Catch::Approx(0.0f));
    CHECK(s.offset_y != Catch::Approx(0.0f));
    sway_reset(s);
    CHECK(s.offset_x == Catch::Approx(0.0f));
    CHECK(s.offset_y == Catch::Approx(0.0f));
}

TEST_CASE("sway: update is deterministic", "[camera][determinism]") {
    const SwayParams p = make_params();
    SwayState a;
    SwayState b;
    sway_init(a);
    sway_init(b);
    for (int i = 0; i < 120; ++i) {
        // Vary the look deltas over the sequence (turn, settle, turn back).
        const f32 yaw   = static_cast<f32>((i % 7) - 3);
        const f32 pitch = static_cast<f32>((i % 5) - 2);
        sway_update(a, p, yaw, pitch, 1.0f / 90.0f);
        sway_update(b, p, yaw, pitch, 1.0f / 90.0f);
    }
    CHECK(a.offset_x == b.offset_x);  // bit-identical after identical sequences
    CHECK(a.offset_y == b.offset_y);
}
