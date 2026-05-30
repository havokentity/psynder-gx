// SPDX-License-Identifier: MIT
// Psynder — lane 14 unit test: equal-power crossfade between two sources
// (Crossfade.h), the music/ambience transition mixer.
//
// Verifies the load-bearing properties of engine/audio/Crossfade.h:
//   (a) equal_power_gains hits the endpoints (t=0 -> 1,0; t=1 -> 0,1) and the
//       midpoint (t=0.5 -> both ~0.7071), and keeps constant power
//       (a^2 + b^2 == 1) across a sweep of t — the no-mid-dip guarantee;
//   (b) the position parameter clamps to [0,1] outside the domain;
//   (c) fader_update advances toward the target without overshoot and converges,
//       and is frame-rate independent in total elapsed time;
//   (d) fader_gains matches equal_power_gains at the fader's position;
//   (e) mix_two blends two constant samples (t=0 -> sample_a, t=1 -> sample_b);
//   (f) a zero / non-finite dt (and bad rate) is guarded — the fader holds; and
//   (g) determinism: identical calls are bit-identical.

#include "audio/Crossfade.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>  // std::sqrt, INFINITY, NAN

using namespace psynder;
using namespace psynder::audio;

TEST_CASE("audio: equal_power_gains hits the endpoints and midpoint", "[audio][crossfade]") {
    f32 a = -1.0f, b = -1.0f;

    // t = 0: full source A.
    equal_power_gains(0.0f, a, b);
    REQUIRE(a == Catch::Approx(1.0f).margin(1e-6f));
    REQUIRE(b == Catch::Approx(0.0f).margin(1e-6f));

    // t = 1: full source B.
    equal_power_gains(1.0f, a, b);
    REQUIRE(a == Catch::Approx(0.0f).margin(1e-6f));
    REQUIRE(b == Catch::Approx(1.0f).margin(1e-6f));

    // t = 0.5: equal, each 1/sqrt(2).
    equal_power_gains(0.5f, a, b);
    const f32 inv_sqrt2 = 1.0f / std::sqrt(2.0f);
    REQUIRE(a == Catch::Approx(inv_sqrt2).margin(1e-6f));
    REQUIRE(b == Catch::Approx(inv_sqrt2).margin(1e-6f));
    REQUIRE(a == Catch::Approx(b).margin(1e-6f));
}

TEST_CASE("audio: equal_power_gains holds constant power across the sweep", "[audio][crossfade]") {
    // Across the whole crossfade the squared gains sum to 1 (constant power) and
    // each gain stays in [0,1]. This is the property that kills the mid-fade dip.
    for (f32 t = 0.0f; t <= 1.0f; t += 0.05f) {
        f32 a = 0.0f, b = 0.0f;
        equal_power_gains(t, a, b);
        INFO("t=" << t << " a=" << a << " b=" << b);
        REQUIRE((a * a + b * b) == Catch::Approx(1.0f).margin(1e-5f));
        REQUIRE(a >= 0.0f);
        REQUIRE(a <= 1.0f);
        REQUIRE(b >= 0.0f);
        REQUIRE(b <= 1.0f);
    }
}

TEST_CASE("audio: equal_power_gains clamps t outside the unit interval", "[audio][crossfade]") {
    f32 a = -1.0f, b = -1.0f;

    // Below 0 behaves like t = 0 (full A).
    equal_power_gains(-2.5f, a, b);
    REQUIRE(a == Catch::Approx(1.0f).margin(1e-6f));
    REQUIRE(b == Catch::Approx(0.0f).margin(1e-6f));

    // Above 1 behaves like t = 1 (full B).
    equal_power_gains(7.0f, a, b);
    REQUIRE(a == Catch::Approx(0.0f).margin(1e-6f));
    REQUIRE(b == Catch::Approx(1.0f).margin(1e-6f));
}

TEST_CASE("audio: fader_init clamps the starting position", "[audio][crossfade]") {
    Fader lo{}, hi{}, mid{};
    fader_init(lo, -3.0f);
    fader_init(hi, 9.0f);
    fader_init(mid, 0.25f);
    REQUIRE(lo.t == Catch::Approx(0.0f).margin(1e-6f));
    REQUIRE(hi.t == Catch::Approx(1.0f).margin(1e-6f));
    REQUIRE(mid.t == Catch::Approx(0.25f).margin(1e-6f));
}

TEST_CASE("audio: fader_update advances toward the target without overshoot", "[audio][crossfade]") {
    // Start at A, drive toward B. One step of rate*dt = 0.5*0.1 = 0.05.
    Fader f{};
    fader_init(f, 0.0f);
    fader_update(f, 1.0f, 0.5f, 0.1f);
    REQUIRE(f.t == Catch::Approx(0.05f).margin(1e-6f));
    REQUIRE(f.t <= 1.0f);

    // Many steps converge to (and stop at) the target without passing it.
    for (int i = 0; i < 1000; ++i) {
        fader_update(f, 1.0f, 0.5f, 0.1f);
        REQUIRE(f.t <= 1.0f);  // never overshoots past the target/ceiling
    }
    REQUIRE(f.t == Catch::Approx(1.0f).margin(1e-6f));

    // It also moves the other direction (B -> A) and lands exactly on target.
    fader_update(f, 0.25f, 100.0f, 1.0f);  // huge step would pass 0.25
    REQUIRE(f.t == Catch::Approx(0.25f).margin(1e-6f));  // clamped to target, no overshoot
}

TEST_CASE("audio: fader_update is frame-rate independent in elapsed time", "[audio][crossfade]") {
    // Same total time at the same rate reaches the same position whether taken
    // in one big step or many small ones (no per-step rounding drift here).
    const f32 rate = 0.5f;  // units of t per second

    Fader coarse{};
    fader_init(coarse, 0.0f);
    fader_update(coarse, 1.0f, rate, 1.0f);  // 1.0 s in one step => +0.5

    Fader fine{};
    fader_init(fine, 0.0f);
    for (int i = 0; i < 10; ++i) {
        fader_update(fine, 1.0f, rate, 0.1f);  // 10 x 0.1 s => +0.5
    }

    REQUIRE(coarse.t == Catch::Approx(0.5f).margin(1e-5f));
    REQUIRE(fine.t == Catch::Approx(coarse.t).margin(1e-5f));
}

TEST_CASE("audio: fader_gains matches equal_power_gains at the position", "[audio][crossfade]") {
    Fader f{};
    fader_init(f, 0.3f);

    f32 fa = 0.0f, fb = 0.0f;
    fader_gains(f, fa, fb);

    f32 ea = 0.0f, eb = 0.0f;
    equal_power_gains(0.3f, ea, eb);

    REQUIRE(fa == Catch::Approx(ea).margin(1e-6f));
    REQUIRE(fb == Catch::Approx(eb).margin(1e-6f));
    REQUIRE((fa * fa + fb * fb) == Catch::Approx(1.0f).margin(1e-5f));
}

TEST_CASE("audio: mix_two blends two constant samples by position", "[audio][crossfade]") {
    const f32 sa = 0.8f;   // source A sample
    const f32 sb = -0.4f;  // source B sample

    // At t = 0 the mix is pure source A.
    Fader at_a{};
    fader_init(at_a, 0.0f);
    REQUIRE(mix_two(sa, sb, at_a) == Catch::Approx(sa).margin(1e-6f));

    // At t = 1 the mix is pure source B.
    Fader at_b{};
    fader_init(at_b, 1.0f);
    REQUIRE(mix_two(sa, sb, at_b) == Catch::Approx(sb).margin(1e-6f));

    // At t = 0.5 it is the equal-power sum of both samples.
    Fader at_mid{};
    fader_init(at_mid, 0.5f);
    f32 ga = 0.0f, gb = 0.0f;
    equal_power_gains(0.5f, ga, gb);
    REQUIRE(mix_two(sa, sb, at_mid) == Catch::Approx(sa * ga + sb * gb).margin(1e-6f));
}

TEST_CASE("audio: fader_update guards against bad timing and rates", "[audio][crossfade]") {
    Fader f{};
    fader_init(f, 0.2f);

    // Zero dt: no time passes, position holds.
    fader_update(f, 1.0f, 0.5f, 0.0f);
    REQUIRE(f.t == Catch::Approx(0.2f).margin(1e-6f));

    // Negative dt: holds.
    fader_update(f, 1.0f, 0.5f, -0.1f);
    REQUIRE(f.t == Catch::Approx(0.2f).margin(1e-6f));

    // Non-finite dt: holds.
    fader_update(f, 1.0f, 0.5f, INFINITY);
    REQUIRE(f.t == Catch::Approx(0.2f).margin(1e-6f));
    fader_update(f, 1.0f, 0.5f, NAN);
    REQUIRE(f.t == Catch::Approx(0.2f).margin(1e-6f));

    // Zero / negative / non-finite rate: holds.
    fader_update(f, 1.0f, 0.0f, 0.1f);
    REQUIRE(f.t == Catch::Approx(0.2f).margin(1e-6f));
    fader_update(f, 1.0f, -2.0f, 0.1f);
    REQUIRE(f.t == Catch::Approx(0.2f).margin(1e-6f));
    fader_update(f, 1.0f, INFINITY, 0.1f);
    REQUIRE(f.t == Catch::Approx(0.2f).margin(1e-6f));
}

TEST_CASE("audio: crossfade is deterministic across identical calls", "[audio][crossfade]") {
    // Same inputs => bit-identical outputs (same-platform determinism).
    f32 a1 = 0.0f, b1 = 0.0f, a2 = 0.0f, b2 = 0.0f;
    equal_power_gains(0.37f, a1, b1);
    equal_power_gains(0.37f, a2, b2);
    REQUIRE(a1 == a2);
    REQUIRE(b1 == b2);

    Fader f1{}, f2{};
    fader_init(f1, 0.1f);
    fader_init(f2, 0.1f);
    fader_update(f1, 0.9f, 0.3f, 0.016f);
    fader_update(f2, 0.9f, 0.3f, 0.016f);
    REQUIRE(f1.t == f2.t);

    Fader m{};
    fader_init(m, 0.42f);
    REQUIRE(mix_two(0.5f, -0.25f, m) == mix_two(0.5f, -0.25f, m));
}
