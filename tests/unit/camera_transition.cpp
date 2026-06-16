// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/camera_transition.cpp — smooth camera pose blend
// (death cam / kill cam / spawn intro). See engine/camera/Transition.h.

#include "camera/Transition.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>    // std::abs
#include <limits>

using namespace psynder;
using namespace psynder::camera;

namespace {

CamPose make_from() {
    CamPose p;
    p.position[0] = 0.0f;
    p.position[1] = 2.0f;
    p.position[2] = 0.0f;
    p.yaw_deg = 10.0f;
    p.pitch_deg = 0.0f;
    return p;
}

CamPose make_to() {
    CamPose p;
    p.position[0] = 10.0f;
    p.position[1] = 6.0f;
    p.position[2] = -4.0f;
    p.yaw_deg = 50.0f;
    p.pitch_deg = -20.0f;
    return p;
}

bool pose_approx_eq(const CamPose& a, const CamPose& b) {
    return a.position[0] == Catch::Approx(b.position[0]) &&
           a.position[1] == Catch::Approx(b.position[1]) &&
           a.position[2] == Catch::Approx(b.position[2]) &&
           a.yaw_deg == Catch::Approx(b.yaw_deg) &&
           a.pitch_deg == Catch::Approx(b.pitch_deg);
}

}  // namespace

TEST_CASE("transition: at elapsed zero the sample equals the from pose", "[camera]") {
    const CamPose from = make_from();
    const CamPose to = make_to();
    TransitionState t;
    transition_start(t, from, to, 1.0f);

    CHECK(transition_active(t));
    const CamPose s = transition_sample(t);   // elapsed still 0
    CHECK(pose_approx_eq(s, from));
}

TEST_CASE("transition: at and after the duration the sample equals the to pose", "[camera]") {
    const CamPose from = make_from();
    const CamPose to = make_to();
    TransitionState t;
    transition_start(t, from, to, 1.0f);

    // Tick exactly to the end.
    transition_tick(t, 1.0f);
    CHECK_FALSE(transition_active(t));           // active latches off at the end
    CHECK(pose_approx_eq(transition_sample(t), to));

    // Ticking past the end stays clamped on `to` and inactive.
    transition_tick(t, 5.0f);
    CHECK_FALSE(transition_active(t));
    CHECK(t.elapsed_s == Catch::Approx(1.0f));   // clamped, did not overrun
    CHECK(pose_approx_eq(transition_sample(t), to));
}

TEST_CASE("transition: smoothstep is exact at zero half and one", "[camera]") {
    CHECK(smoothstep01(0.0f) == Catch::Approx(0.0f));
    CHECK(smoothstep01(0.5f) == Catch::Approx(0.5f));
    CHECK(smoothstep01(1.0f) == Catch::Approx(1.0f));
}

TEST_CASE("transition: smoothstep clamps outside the unit range", "[camera]") {
    CHECK(smoothstep01(-1.0f) == Catch::Approx(0.0f));   // below 0 clamps to 0
    CHECK(smoothstep01(-100.0f) == Catch::Approx(0.0f));
    CHECK(smoothstep01(2.0f) == Catch::Approx(1.0f));    // above 1 clamps to 1
    CHECK(smoothstep01(100.0f) == Catch::Approx(1.0f));
}

TEST_CASE("transition: smoothstep is monotone and eases gently near the ends", "[camera]") {
    // Strictly increasing across the interior, and below the linear line in the
    // first half (slow start), above it in the second half (slow finish).
    CHECK(smoothstep01(0.25f) < 0.25f);   // eases in below the diagonal
    CHECK(smoothstep01(0.75f) > 0.75f);   // eases out above the diagonal
    f32 prev = smoothstep01(0.0f);
    for (int i = 1; i <= 20; ++i) {
        const f32 x = static_cast<f32>(i) / 20.0f;
        const f32 y = smoothstep01(x);
        CHECK(y >= prev);                 // non-decreasing
        prev = y;
    }
}

TEST_CASE("transition: position lerps along the eased curve", "[camera]") {
    const CamPose from = make_from();
    const CamPose to = make_to();
    TransitionState t;
    transition_start(t, from, to, 1.0f);

    // Halfway in time: smoothstep(0.5) == 0.5, so position is the midpoint.
    transition_tick(t, 0.5f);
    const CamPose mid = transition_sample(t);
    CHECK(mid.position[0] == Catch::Approx(5.0f));    // (0 + 10) / 2
    CHECK(mid.position[1] == Catch::Approx(4.0f));    // (2 + 6) / 2
    CHECK(mid.position[2] == Catch::Approx(-2.0f));   // (0 + -4) / 2
    CHECK(mid.pitch_deg == Catch::Approx(-10.0f));    // (0 + -20) / 2
}

TEST_CASE("transition: yaw takes the short way across the plus minus 180 wrap", "[camera]") {
    // from 170 to -170 is a 20 degree move that should pass through +180, NOT a
    // 340 degree sweep the long way through 0.
    CamPose from;
    from.yaw_deg = 170.0f;
    CamPose to;
    to.yaw_deg = -170.0f;

    TransitionState t;
    transition_start(t, from, to, 1.0f);

    // Quarter of the way: +170 + 0.25 * (+20) wrapped -> +175 (still short arc).
    transition_tick(t, 0.25f);  // smoothstep(0.25) ~ 0.15625, well short of mid
    const f32 q = transition_sample(t).yaw_deg;
    CHECK(q > 170.0f);          // moved toward +180, did not dive toward 0
    CHECK(q <= 180.0f);

    // Reset and sample the exact midpoint: smoothstep(0.5) == 0.5 => +180 (which
    // wraps to -180). Either representation is "near 180", the seam — NOT 0.
    TransitionState t2;
    transition_start(t2, from, to, 1.0f);
    transition_tick(t2, 0.5f);
    const f32 m = transition_sample(t2).yaw_deg;
    const f32 dist_to_seam = (m >= 0.0f) ? (180.0f - m) : (180.0f + m);
    CHECK(dist_to_seam == Catch::Approx(0.0f).margin(1e-3f));  // at the +/-180 seam
    CHECK(std::abs(m) > 90.0f);                                // nowhere near 0
}

TEST_CASE("transition: yaw endpoints are exact across the wrap", "[camera]") {
    CamPose from;
    from.yaw_deg = 170.0f;
    CamPose to;
    to.yaw_deg = -170.0f;

    TransitionState t;
    transition_start(t, from, to, 1.0f);
    CHECK(transition_sample(t).yaw_deg == Catch::Approx(170.0f));  // elapsed 0 => from
    transition_tick(t, 1.0f);
    CHECK(transition_sample(t).yaw_deg == Catch::Approx(-170.0f)); // end => to
}

TEST_CASE("transition: a zero duration snaps instantly to the to pose", "[camera]") {
    const CamPose from = make_from();
    const CamPose to = make_to();
    TransitionState t;
    transition_start(t, from, to, 0.0f);   // degenerate duration

    CHECK_FALSE(transition_active(t));               // never enters the blend
    CHECK(pose_approx_eq(transition_sample(t), to));  // already at the target
}

TEST_CASE("transition: a negative duration snaps instantly to the to pose", "[camera]") {
    const CamPose from = make_from();
    const CamPose to = make_to();
    TransitionState t;
    transition_start(t, from, to, -2.0f);   // degenerate duration

    CHECK_FALSE(transition_active(t));
    CHECK(pose_approx_eq(transition_sample(t), to));
}

TEST_CASE("transition: a zero or non-finite dt leaves the clock untouched", "[camera]") {
    const CamPose from = make_from();
    const CamPose to = make_to();
    TransitionState t;
    transition_start(t, from, to, 1.0f);

    // Advance part-way so there is a non-trivial clock to protect.
    transition_tick(t, 0.3f);
    const f32 before = t.elapsed_s;
    CHECK(before == Catch::Approx(0.3f));

    transition_tick(t, 0.0f);    // zero dt -> no advance
    CHECK(t.elapsed_s == before);
    transition_tick(t, -1.0f);   // negative dt -> no advance
    CHECK(t.elapsed_s == before);
    const f32 inf = std::numeric_limits<f32>::infinity();
    transition_tick(t, inf);     // non-finite dt -> no advance
    CHECK(t.elapsed_s == before);
    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    transition_tick(t, nan);     // non-finite dt -> no advance
    CHECK(t.elapsed_s == before);

    CHECK(transition_active(t));  // still mid-blend, untouched
}

TEST_CASE("transition: ticking an inactive transition is a no-op", "[camera]") {
    const CamPose from = make_from();
    const CamPose to = make_to();
    TransitionState t;
    transition_start(t, from, to, 1.0f);
    transition_tick(t, 1.0f);              // run it to completion
    CHECK_FALSE(transition_active(t));
    const f32 end = t.elapsed_s;

    transition_tick(t, 0.5f);              // further ticks do nothing
    CHECK(t.elapsed_s == end);
    CHECK_FALSE(transition_active(t));
}

TEST_CASE("transition: the blend is deterministic", "[camera][determinism]") {
    const CamPose from = make_from();
    const CamPose to = make_to();
    TransitionState a;
    TransitionState b;
    transition_start(a, from, to, 1.0f);
    transition_start(b, from, to, 1.0f);

    for (int i = 0; i < 30; ++i) {
        transition_tick(a, 1.0f / 60.0f);
        transition_tick(b, 1.0f / 60.0f);
        const CamPose pa = transition_sample(a);
        const CamPose pb = transition_sample(b);
        // Bit-identical after identical input sequences.
        CHECK(pa.position[0] == pb.position[0]);
        CHECK(pa.position[1] == pb.position[1]);
        CHECK(pa.position[2] == pb.position[2]);
        CHECK(pa.yaw_deg == pb.yaw_deg);
        CHECK(pa.pitch_deg == pb.pitch_deg);
    }
    CHECK(a.elapsed_s == b.elapsed_s);
    CHECK(a.active == b.active);
}
