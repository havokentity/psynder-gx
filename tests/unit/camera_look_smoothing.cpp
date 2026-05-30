// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/camera_look_smoothing.cpp — exponential low-pass on raw mouse-look
// deltas (input smoothing). See engine/camera/LookSmoothing.h.

#include "camera/LookSmoothing.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <limits>

using namespace psynder;
using namespace psynder::camera;

namespace {
LookSmoothParams make_params(f32 smoothing) {
    LookSmoothParams p;
    p.smoothing = smoothing;
    return p;
}
}  // namespace

TEST_CASE("look_smooth: init zeroes the velocities", "[camera]") {
    LookSmoothState s;
    s.yaw_vel_deg = 7.0f;
    s.pitch_vel_deg = -3.0f;
    look_smooth_init(s);
    CHECK(s.yaw_vel_deg == Catch::Approx(0.0f));
    CHECK(s.pitch_vel_deg == Catch::Approx(0.0f));
}

TEST_CASE("look_smooth: smoothing 0 is exact passthrough", "[camera]") {
    const LookSmoothParams p = make_params(0.0f);  // alpha == 1
    LookSmoothState s;
    look_smooth_init(s);

    // Bit-exact: out must EQUAL raw on every axis, every call, regardless of the
    // carried velocity (which simply gets overwritten to raw each step).
    f32 oy = -999.0f, op = -999.0f;
    look_smooth(s, p, 5.0f, -2.0f, oy, op);
    CHECK(oy == 5.0f);
    CHECK(op == -2.0f);
    CHECK(s.yaw_vel_deg == 5.0f);
    CHECK(s.pitch_vel_deg == -2.0f);

    look_smooth(s, p, -11.5f, 8.25f, oy, op);
    CHECK(oy == -11.5f);
    CHECK(op == 8.25f);
}

TEST_CASE("look_smooth: a step input is approached gradually", "[camera]") {
    const LookSmoothParams p = make_params(0.5f);  // alpha == 0.5
    LookSmoothState s;
    look_smooth_init(s);

    // First call against a step of 10: out rises toward 10 but LAGS (does not
    // jump straight there) because alpha < 1.
    f32 oy = 0.0f, op = 0.0f;
    look_smooth(s, p, 10.0f, 0.0f, oy, op);
    CHECK(oy > 0.0f);    // it moved toward the step
    CHECK(oy < 10.0f);   // ... but lagged behind it
    CHECK(op == Catch::Approx(0.0f));  // the still axis stays put

    // Subsequent calls with the SAME held input keep climbing, monotonically,
    // never overshooting the target.
    f32 prev = oy;
    for (int i = 0; i < 40; ++i) {
        look_smooth(s, p, 10.0f, 0.0f, oy, op);
        CHECK(oy >= prev - 1e-4f);   // monotonic toward 10
        CHECK(oy <= 10.0f + 1e-4f);  // never overshoots
        prev = oy;
    }
}

TEST_CASE("look_smooth: a sustained constant input converges", "[camera]") {
    const LookSmoothParams p = make_params(0.7f);
    LookSmoothState s;
    look_smooth_init(s);

    f32 oy = 0.0f, op = 0.0f;
    for (int i = 0; i < 500; ++i) {
        look_smooth(s, p, 3.0f, -4.0f, oy, op);
    }
    // out converges to the held raw value on each axis.
    CHECK(oy == Catch::Approx(3.0f).margin(0.001f));
    CHECK(op == Catch::Approx(-4.0f).margin(0.001f));
}

TEST_CASE("look_smooth: smoothing clamps and never freezes updates", "[camera]") {
    // smoothing >= 1 clamps to 0.999 so alpha is floored at 0.001 (> 0): the
    // smoother crawls but STILL creeps toward the input every call.
    const LookSmoothParams p = make_params(5.0f);  // clamps to 0.999
    LookSmoothState s;
    look_smooth_init(s);

    f32 oy = 0.0f, op = 0.0f;
    look_smooth(s, p, 100.0f, 0.0f, oy, op);
    CHECK(oy > 0.0f);     // it DID move — not frozen
    CHECK(oy < 100.0f);   // ... by only a sliver (heavy smoothing)

    // Over enough calls even the crawling smoother makes clear progress.
    const f32 after_one = oy;
    for (int i = 0; i < 50; ++i) look_smooth(s, p, 100.0f, 0.0f, oy, op);
    CHECK(oy > after_one);
}

TEST_CASE("look_smooth: non-finite raw is treated as zero", "[camera]") {
    const LookSmoothParams p = make_params(0.5f);
    LookSmoothState s;
    look_smooth_init(s);

    const f32 inf = std::numeric_limits<f32>::infinity();
    const f32 nan = std::numeric_limits<f32>::quiet_NaN();

    f32 oy = 0.0f, op = 0.0f;
    look_smooth(s, p, nan, inf, oy, op);   // both bogus -> treated as 0
    CHECK(oy == Catch::Approx(0.0f));      // no NaN leaked in, no move
    CHECK(op == Catch::Approx(0.0f));
    CHECK(s.yaw_vel_deg == Catch::Approx(0.0f));
    CHECK(s.pitch_vel_deg == Catch::Approx(0.0f));

    // A good axis still smooths normally even when the other is bogus.
    look_smooth(s, p, 8.0f, nan, oy, op);
    CHECK(oy > 0.0f);
    CHECK(oy < 8.0f);
    CHECK(op == Catch::Approx(0.0f));  // bogus pitch -> stays settled
}

TEST_CASE("look_smooth: reset zeroes the velocities", "[camera]") {
    const LookSmoothParams p = make_params(0.6f);
    LookSmoothState s;
    look_smooth_init(s);

    f32 oy = 0.0f, op = 0.0f;
    for (int i = 0; i < 10; ++i) look_smooth(s, p, 9.0f, -9.0f, oy, op);
    CHECK(s.yaw_vel_deg != Catch::Approx(0.0f));  // it had carried velocity

    look_smooth_reset(s);
    CHECK(s.yaw_vel_deg == Catch::Approx(0.0f));
    CHECK(s.pitch_vel_deg == Catch::Approx(0.0f));
}

TEST_CASE("look_smooth: filtering is deterministic", "[camera][determinism]") {
    const LookSmoothParams p = make_params(0.65f);
    LookSmoothState a; look_smooth_init(a);
    LookSmoothState b; look_smooth_init(b);

    f32 ay = 0.0f, ap = 0.0f, by = 0.0f, bp = 0.0f;
    for (int i = 0; i < 100; ++i) {
        const f32 ry = static_cast<f32>(i % 7) - 3.0f;
        const f32 rp = static_cast<f32>(i % 5) - 2.0f;
        look_smooth(a, p, ry, rp, ay, ap);
        look_smooth(b, p, ry, rp, by, bp);
    }
    // Same (state, params, inputs) => bit-identical, not just approx.
    CHECK(ay == by);
    CHECK(ap == bp);
    CHECK(a.yaw_vel_deg == b.yaw_vel_deg);
    CHECK(a.pitch_vel_deg == b.pitch_vel_deg);
}
