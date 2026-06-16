// SPDX-License-Identifier: MIT
//
// tests/unit/audio_distance_delay.cpp — speed-of-sound delay + air absorption.

#include "audio/DistanceDelay.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace psynder;
using namespace psynder::audio;

TEST_CASE("distance delay: propagation delay is distance over speed", "[audio]") {
    CHECK(propagation_delay_s(343.0f) == Catch::Approx(1.0f));       // ~1 s at 343 m
    CHECK(propagation_delay_s(686.0f) == Catch::Approx(2.0f));
    CHECK(propagation_delay_s(0.0f) == Catch::Approx(0.0f));
    // Custom speed of sound.
    CHECK(propagation_delay_s(100.0f, 1000.0f) == Catch::Approx(0.1f));
}

TEST_CASE("distance delay: degenerate inputs are guarded", "[audio]") {
    CHECK(propagation_delay_s(-50.0f) == Catch::Approx(0.0f));   // negative distance
    CHECK(propagation_delay_s(100.0f, 0.0f) == Catch::Approx(0.0f));   // zero speed
    CHECK(propagation_delay_s(100.0f, -5.0f) == Catch::Approx(0.0f));  // negative speed
}

TEST_CASE("distance delay: delay in samples rounds correctly", "[audio]") {
    // 343 m at 48 kHz => ~1 s => ~48000 samples.
    CHECK(delay_samples(343.0f, 48000) == 48000u);
    CHECK(delay_samples(0.0f, 48000) == 0u);
    // 17.15 m / 343 = 0.05 s * 48000 = 2400 samples.
    CHECK(delay_samples(17.15f, 48000) == 2400u);
}

TEST_CASE("distance delay: air absorption gain decreases with distance", "[audio]") {
    CHECK(air_absorption_gain(0.0f, 0.01f) == Catch::Approx(1.0f));  // unity at source
    const f32 g_near = air_absorption_gain(10.0f, 0.01f);
    const f32 g_far = air_absorption_gain(100.0f, 0.01f);
    CHECK(g_near < 1.0f);
    CHECK(g_far < g_near);   // monotonic decreasing
    CHECK(g_far > 0.0f);     // stays in (0,1]
    CHECK(g_far <= 1.0f);
    // 1 / (1 + 0.01*100) = 1/2 = 0.5.
    CHECK(air_absorption_gain(100.0f, 0.01f) == Catch::Approx(0.5f));
    // Negative inputs clamp to "no absorption" (unity).
    CHECK(air_absorption_gain(-5.0f, 0.01f) == Catch::Approx(1.0f));
    CHECK(air_absorption_gain(50.0f, -1.0f) == Catch::Approx(1.0f));
}

TEST_CASE("distance delay: distant_arrival bundles consistent cues", "[audio]") {
    const ArrivalCue c = distant_arrival(343.0f, 48000, 0.01f);
    CHECK(c.delay_s == Catch::Approx(propagation_delay_s(343.0f)));
    CHECK(c.delay_samples == delay_samples(343.0f, 48000));
    CHECK(c.absorption_gain == Catch::Approx(air_absorption_gain(343.0f, 0.01f)));
}

TEST_CASE("distance delay: outputs are deterministic", "[audio][determinism]") {
    for (int i = 0; i < 32; ++i) {
        const f32 d = static_cast<f32>(i) * 11.0f;
        CHECK(propagation_delay_s(d) == propagation_delay_s(d));
        CHECK(delay_samples(d, 44100) == delay_samples(d, 44100));
        CHECK(air_absorption_gain(d, 0.02f) == air_absorption_gain(d, 0.02f));
    }
}
