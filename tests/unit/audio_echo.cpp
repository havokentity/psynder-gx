// SPDX-License-Identifier: MIT
// Psynder — lane 14 unit test: feedback echo (delay line) parameter model
// (Echo.h), the authoring-to-tap-schedule front-end the mixer plays.
//
// Verifies the load-bearing properties of engine/audio/Echo.h:
//   (a) echo_delay_samples rounds delay_time * sample_rate to the nearest
//       sample (0.25 s @ 48 kHz => 12000), guards non-positive delays;
//   (b) echo_tap_gain decays geometrically under the documented convention
//       (tap 0 = 1, tap 1 = feedback, tap 2 = feedback^2), and a feedback >= 1
//       is clamped so the taps still decay;
//   (c) echo_audible_taps reports more taps for higher feedback, 0 for
//       feedback 0, and is consistent with echo_tap_gain crossing the floor;
//   (d) echo_tap_time_s = tap_index * delay_time;
//   (e) determinism: identical calls are bit-identical.

#include "audio/Echo.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace psynder;
using namespace psynder::audio;

TEST_CASE("audio: echo_delay_samples rounds delay time to mixer samples", "[audio][echo]") {
    // 0.25 s at 48 kHz is exactly 12000 samples.
    REQUIRE(echo_delay_samples(0.25f, 48000u) == 12000u);

    // 1 s at 44.1 kHz is 44100 samples.
    REQUIRE(echo_delay_samples(1.0f, 44100u) == 44100u);

    // Rounds to the nearest sample: 0.5 / 48000 s -> 24 samples exactly,
    // a hair under still rounds to 24, a hair over to 25.
    REQUIRE(echo_delay_samples(0.0005f, 48000u) == 24u);

    // Non-positive (or zero-rate) inputs guard to 0, never negative.
    REQUIRE(echo_delay_samples(0.0f, 48000u) == 0u);
    REQUIRE(echo_delay_samples(-0.25f, 48000u) == 0u);
    REQUIRE(echo_delay_samples(0.25f, 0u) == 0u);
}

TEST_CASE("audio: echo_tap_gain decays geometrically from a unity dry tap", "[audio][echo]") {
    const f32 fb = 0.5f;

    // Tap 0 is the dry signal: always exactly 1.0 regardless of feedback.
    REQUIRE(echo_tap_gain(fb, 0u) == Catch::Approx(1.0f).margin(1e-6f));
    REQUIRE(echo_tap_gain(0.9f, 0u) == Catch::Approx(1.0f).margin(1e-6f));

    // Tap n has gain feedback^n: tap 1 = fb, tap 2 = fb^2, tap 3 = fb^3.
    REQUIRE(echo_tap_gain(fb, 1u) == Catch::Approx(0.5f).margin(1e-6f));
    REQUIRE(echo_tap_gain(fb, 2u) == Catch::Approx(0.25f).margin(1e-6f));
    REQUIRE(echo_tap_gain(fb, 3u) == Catch::Approx(0.125f).margin(1e-6f));

    // Strictly decreasing tail.
    REQUIRE(echo_tap_gain(fb, 2u) < echo_tap_gain(fb, 1u));
    REQUIRE(echo_tap_gain(fb, 3u) < echo_tap_gain(fb, 2u));
}

TEST_CASE("audio: echo_tap_gain clamps a feedback at or above one so taps still decay", "[audio][echo]") {
    // A feedback of exactly 1 (or more) would sustain forever; it is clamped
    // to kEchoMaxFeedback (< 1) so successive taps still decay.
    const f32 g1 = echo_tap_gain(1.0f, 1u);
    const f32 g2 = echo_tap_gain(1.0f, 2u);
    REQUIRE(g1 < 1.0f);
    REQUIRE(g2 < g1);
    REQUIRE(g1 == Catch::Approx(kEchoMaxFeedback).margin(1e-6f));

    // An above-unity feedback clamps to the same ceiling as exactly-unity.
    REQUIRE(echo_tap_gain(2.5f, 1u) == Catch::Approx(echo_tap_gain(1.0f, 1u)).margin(1e-6f));
}

TEST_CASE("audio: echo_audible_taps grows with feedback and is zero with none", "[audio][echo]") {
    const f32 floor = 0.01f;  // -40 dB linear

    // No feedback => no echo survives the floor.
    REQUIRE(echo_audible_taps(0.0f, floor) == 0u);

    // Higher feedback keeps more taps above the floor than lower feedback.
    const u32 low  = echo_audible_taps(0.3f, floor);
    const u32 mid  = echo_audible_taps(0.6f, floor);
    const u32 high = echo_audible_taps(0.9f, floor);
    INFO("low=" << low << " mid=" << mid << " high=" << high);
    REQUIRE(mid > low);
    REQUIRE(high > mid);

    // The count is capped at the sane maximum even for near-unity feedback.
    REQUIRE(echo_audible_taps(0.999f, floor) <= kEchoMaxTaps);

    // A non-positive floor admits every tap up to the cap.
    REQUIRE(echo_audible_taps(0.5f, 0.0f) == kEchoMaxTaps);
}

TEST_CASE("audio: echo_audible_taps is consistent with echo_tap_gain crossing the floor", "[audio][echo]") {
    const f32 fb = 0.6f;
    const f32 floor = 0.01f;

    const u32 n = echo_audible_taps(fb, floor);
    REQUIRE(n > 0u);
    REQUIRE(n <= kEchoMaxTaps);

    // Tap n (the last counted echo) is at or above the floor; tap n+1 is below.
    // (Under the convention tap n is audible for n in [1, count].)
    REQUIRE(echo_tap_gain(fb, n) >= floor);
    REQUIRE(echo_tap_gain(fb, n + 1u) < floor);
}

TEST_CASE("audio: echo_tap_time_s is tap index times the delay length", "[audio][echo]") {
    const EchoParams p{0.2f, 0.5f, 0.4f};

    // Tap 0 (dry) arrives at time 0; each later tap one more delay on.
    REQUIRE(echo_tap_time_s(p, 0u) == Catch::Approx(0.0f).margin(1e-6f));
    REQUIRE(echo_tap_time_s(p, 1u) == Catch::Approx(0.2f).margin(1e-6f));
    REQUIRE(echo_tap_time_s(p, 2u) == Catch::Approx(0.4f).margin(1e-6f));
    REQUIRE(echo_tap_time_s(p, 5u) == Catch::Approx(1.0f).margin(1e-6f));

    // A negative delay length is treated as 0 (no spread).
    const EchoParams bad{-0.2f, 0.5f, 0.4f};
    REQUIRE(echo_tap_time_s(bad, 3u) == Catch::Approx(0.0f).margin(1e-6f));
}

TEST_CASE("audio: echo parameters are deterministic across identical calls", "[audio][echo]") {
    // Same inputs => bit-identical outputs (same-platform determinism).
    REQUIRE(echo_delay_samples(0.31f, 48000u) == echo_delay_samples(0.31f, 48000u));
    REQUIRE(echo_tap_gain(0.7f, 4u) == echo_tap_gain(0.7f, 4u));
    REQUIRE(echo_audible_taps(0.7f, 0.01f) == echo_audible_taps(0.7f, 0.01f));

    const EchoParams p{0.17f, 0.65f, 0.5f};
    REQUIRE(echo_tap_time_s(p, 6u) == echo_tap_time_s(p, 6u));
}
