// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/camera_recoil.cpp
//
// Lane 16 — deterministic weapon recoil / spray-pattern model. Validates the
// contract documented in engine/camera/Recoil.h:
//   - Firing N shots climbs pitch_offset_deg monotonically up to (never past)
//     max_pitch_deg, and shot_index increments per shot.
//   - The horizontal yaw kick is deterministic for a given (seed, shot_index)
//     and takes BOTH signs across a spray (the weave).
//   - recoil_recover decays both offsets toward 0 by recovery*dt and clamps at
//     0 without overshoot; once fully recovered shot_index resets to 0.
//   - recoil_reset zeroes the state.
//   - Two identical fire+recover sequences produce bit-identical RecoilState
//     (== on the f32 fields — the determinism / replay-safety guarantee).
//   - Changing the seed changes the horizontal pattern but NOT the pitch climb.

#include "camera/Recoil.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <type_traits>

using namespace psynder;
using namespace psynder::camera;

namespace {

// A representative spray tuning shared by most cases.
RecoilPattern make_pattern() noexcept {
    RecoilPattern p{};
    p.pitch_per_shot_deg = 1.0f;
    p.yaw_amplitude_deg  = 0.5f;
    p.recovery_per_s_deg = 8.0f;
    p.max_pitch_deg      = 12.0f;
    p.seed               = 0xC0FFEEu;
    return p;
}

}  // namespace

TEST_CASE("recoil: state is a trivially copyable POD", "[camera]") {
    REQUIRE(std::is_trivially_copyable_v<RecoilState>);
    REQUIRE(std::is_trivially_copyable_v<RecoilPattern>);
    REQUIRE(sizeof(RecoilState) == 12);
}

TEST_CASE("recoil: firing climbs pitch monotonically up to the cap",
          "[camera]") {
    const RecoilPattern p = make_pattern();
    RecoilState s{};

    f32 prev = s.pitch_offset_deg;
    // With pitch_per_shot=1 and a cap of 12, the climb saturates at shot 12.
    for (u32 i = 0; i < 30; ++i) {
        recoil_fire(s, p);

        // shot_index advances exactly one per shot.
        REQUIRE(s.shot_index == i + 1u);

        // Pitch is non-decreasing and never exceeds the cap.
        REQUIRE(s.pitch_offset_deg >= prev);
        REQUIRE(s.pitch_offset_deg <= p.max_pitch_deg);

        prev = s.pitch_offset_deg;
    }

    // After enough shots it pins at exactly the cap.
    REQUIRE(s.pitch_offset_deg == Catch::Approx(p.max_pitch_deg));

    // recoil_pitch read helper agrees with the stored field.
    REQUIRE(recoil_pitch(s) == s.pitch_offset_deg);

    // The early climb is exactly pitch_per_shot per shot (pre-saturation).
    RecoilState s2{};
    recoil_fire(s2, p);
    REQUIRE(s2.pitch_offset_deg == Catch::Approx(p.pitch_per_shot_deg));
    recoil_fire(s2, p);
    REQUIRE(s2.pitch_offset_deg == Catch::Approx(2.0f * p.pitch_per_shot_deg));
}

TEST_CASE("recoil: horizontal weave takes both signs and is deterministic "
          "per shot index",
          "[camera]") {
    const RecoilPattern p = make_pattern();

    // The per-shot yaw DELTA = yaw_offset after this shot minus before. Walk a
    // spray and record the sign of each delta; the weave must go both ways.
    RecoilState s{};
    bool saw_positive = false;
    bool saw_negative = false;
    for (u32 i = 0; i < 16; ++i) {
        const f32 before = s.yaw_offset_deg;
        recoil_fire(s, p);
        const f32 delta = s.yaw_offset_deg - before;
        if (delta > 0.0f) saw_positive = true;
        if (delta < 0.0f) saw_negative = true;
    }
    REQUIRE(saw_positive);
    REQUIRE(saw_negative);

    // Determinism per (seed, shot_index): two states fired in lockstep produce
    // identical yaw at every step.
    RecoilState a{};
    RecoilState b{};
    for (u32 i = 0; i < 16; ++i) {
        recoil_fire(a, p);
        recoil_fire(b, p);
        REQUIRE(a.yaw_offset_deg == b.yaw_offset_deg);
    }

    // The yaw kick stays within +/- the amplitude per shot (signed_unit is in
    // [-1, 1], scaled by yaw_amplitude_deg), so the accumulated yaw can never
    // grow faster than amplitude * shots.
    RecoilState c{};
    for (u32 i = 0; i < 8; ++i) {
        const f32 before = c.yaw_offset_deg;
        recoil_fire(c, p);
        const f32 delta = c.yaw_offset_deg - before;
        REQUIRE(std::fabs(delta) <= p.yaw_amplitude_deg + 1e-6f);
    }
}

TEST_CASE("recoil: recover decays both offsets toward zero without overshoot",
          "[camera]") {
    const RecoilPattern p = make_pattern();
    RecoilState s{};

    // Build up some kick.
    for (u32 i = 0; i < 5; ++i) recoil_fire(s, p);
    const f32 pitch_before = s.pitch_offset_deg;
    REQUIRE(pitch_before > 0.0f);

    // One small recovery step: pitch drops by exactly recovery*dt.
    const f32 dt   = 0.1f;
    const f32 step = p.recovery_per_s_deg * dt;   // 0.8 deg
    recoil_recover(s, p, dt);
    REQUIRE(s.pitch_offset_deg == Catch::Approx(pitch_before - step));

    // Yaw also moved strictly toward zero (its magnitude shrank, sign held or
    // it landed on zero).
    // (We only assert the no-overshoot invariant below across full recovery.)

    // Drive recovery to completion with a big dt; both offsets clamp at exactly
    // zero (no negative overshoot) and shot_index resets.
    recoil_recover(s, p, 100.0f);
    REQUIRE(s.pitch_offset_deg == 0.0f);
    REQUIRE(s.yaw_offset_deg   == 0.0f);
    REQUIRE(s.shot_index       == 0u);

    // A negative yaw offset also recovers UP to zero without crossing it. Find
    // a shot whose weave is negative, then recover it.
    RecoilState neg{};
    // Fire until yaw is negative (the weave guarantees both signs appear).
    for (u32 i = 0; i < 64 && neg.yaw_offset_deg >= 0.0f; ++i) {
        recoil_fire(neg, p);
    }
    REQUIRE(neg.yaw_offset_deg < 0.0f);
    recoil_recover(neg, p, 100.0f);
    REQUIRE(neg.yaw_offset_deg == 0.0f);

    // Non-positive dt is a no-op.
    RecoilState s2{};
    recoil_fire(s2, p);
    const RecoilState snapshot = s2;
    recoil_recover(s2, p, 0.0f);
    REQUIRE(s2.pitch_offset_deg == snapshot.pitch_offset_deg);
    REQUIRE(s2.yaw_offset_deg   == snapshot.yaw_offset_deg);
    REQUIRE(s2.shot_index       == snapshot.shot_index);
    recoil_recover(s2, p, -1.0f);
    REQUIRE(s2.pitch_offset_deg == snapshot.pitch_offset_deg);
}

TEST_CASE("recoil: partial recovery does not reset the shot index", "[camera]") {
    const RecoilPattern p = make_pattern();
    RecoilState s{};
    for (u32 i = 0; i < 4; ++i) recoil_fire(s, p);
    REQUIRE(s.shot_index == 4u);

    // A small recovery step that does NOT bring both offsets to zero leaves the
    // spray position untouched, so a follow-up shot continues the pattern.
    recoil_recover(s, p, 0.01f);
    REQUIRE(s.shot_index == 4u);
    REQUIRE((s.pitch_offset_deg > 0.0f || s.yaw_offset_deg != 0.0f));
}

TEST_CASE("recoil: reset zeroes the whole state", "[camera]") {
    const RecoilPattern p = make_pattern();
    RecoilState s{};
    for (u32 i = 0; i < 7; ++i) recoil_fire(s, p);
    REQUIRE(s.shot_index > 0u);

    recoil_reset(s);
    REQUIRE(s.pitch_offset_deg == 0.0f);
    REQUIRE(s.yaw_offset_deg   == 0.0f);
    REQUIRE(s.shot_index       == 0u);
}

TEST_CASE("recoil: identical fire+recover sequences are bit-identical "
          "(determinism)",
          "[camera]") {
    const RecoilPattern p = make_pattern();

    auto run = [&](RecoilState& s) {
        // Interleave fires and recovery steps to exercise the full path.
        for (u32 i = 0; i < 12; ++i) {
            recoil_fire(s, p);
            recoil_recover(s, p, 0.02f);
        }
    };

    RecoilState a{};
    RecoilState b{};
    run(a);
    run(b);

    // Bit-identical on every field (== on the raw f32s, not Approx).
    REQUIRE(a.pitch_offset_deg == b.pitch_offset_deg);
    REQUIRE(a.yaw_offset_deg   == b.yaw_offset_deg);
    REQUIRE(a.shot_index       == b.shot_index);
}

TEST_CASE("recoil: seed changes the horizontal pattern but not the pitch climb",
          "[camera]") {
    RecoilPattern p1 = make_pattern();
    RecoilPattern p2 = make_pattern();
    p1.seed = 0x11111111u;
    p2.seed = 0x22222222u;

    RecoilState s1{};
    RecoilState s2{};

    bool yaw_differs = false;
    for (u32 i = 0; i < 16; ++i) {
        recoil_fire(s1, p1);
        recoil_fire(s2, p2);

        // The vertical climb is seed-independent: identical at every step.
        REQUIRE(s1.pitch_offset_deg == s2.pitch_offset_deg);
        REQUIRE(s1.shot_index == s2.shot_index);

        if (s1.yaw_offset_deg != s2.yaw_offset_deg) yaw_differs = true;
    }

    // ...but the horizontal weave diverges (different seed -> different jitter).
    REQUIRE(yaw_differs);
}
