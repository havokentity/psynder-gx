// SPDX-License-Identifier: MIT
// Psynder — lane 12 unit test: HRTF azimuth produces the expected ITD.
//
// We verify the Woodworth ITD formula directly off the algorithmic core
// from `audio/internal/MixerCore.h`. This is the deliverable's load-bearing
// HRTF property — a sound at +90° azimuth must arrive at the right ear
// roughly head-radius / speed-of-sound seconds before the left ear.

#include "audio/internal/MixerCore.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

TEST_CASE("audio: HRTF ITD is zero on-axis", "[audio][hrtf]") {
    using namespace psynder::audio::detail;
    const psynder::f32 itd = itd_seconds(0.0f);
    REQUIRE(std::fabs(itd) < 1e-6f);
}

TEST_CASE("audio: HRTF ITD at +90 deg matches Woodworth formula", "[audio][hrtf]") {
    using namespace psynder::audio::detail;
    // Woodworth: ITD = r/c * (sin θ + θ)
    // At θ = π/2 → ITD = r/c * (1 + π/2) ≈ 0.0875 / 343 * 2.5708 ≈ 6.56e-4 s
    const psynder::f32 expected = (kHeadRadius / kSpeedOfSound) *
                                  (1.0f + psynder::math::kHalfPi);
    const psynder::f32 itd = itd_seconds(psynder::math::kHalfPi);
    INFO("expected ITD: " << expected << ", actual: " << itd);
    REQUIRE(itd > 0.0f);                                // right-of-listener → positive
    REQUIRE(std::fabs(itd - expected) < 1e-5f);
}

TEST_CASE("audio: HRTF ITD is anti-symmetric across the median plane", "[audio][hrtf]") {
    using namespace psynder::audio::detail;
    for (psynder::f32 az : {0.25f, 0.7f, 1.2f, psynder::math::kHalfPi}) {
        const psynder::f32 right = itd_seconds(+az);
        const psynder::f32 left  = itd_seconds(-az);
        INFO("azimuth=" << az);
        REQUIRE(std::fabs(right + left) < 1e-6f);
    }
}

TEST_CASE("audio: HRTF integer delays at 48 kHz at +90 deg give ~32 samples", "[audio][hrtf]") {
    using namespace psynder::audio::detail;
    // 6.56e-4 s * 48000 ≈ 31.5 → rounds to 32 samples.
    psynder::u32 left = 0, right = 0;
    itd_to_delays(psynder::math::kHalfPi, /*sr*/ 48000u, left, right);
    INFO("left delay: " << left << ", right delay: " << right);
    REQUIRE(right == 0u);              // ipsilateral arrives immediately
    REQUIRE(left  >= 30u);
    REQUIRE(left  <= 34u);

    // And the mirrored case
    itd_to_delays(-psynder::math::kHalfPi, /*sr*/ 48000u, left, right);
    REQUIRE(left == 0u);
    REQUIRE(right >= 30u);
    REQUIRE(right <= 34u);
}

TEST_CASE("audio: minimal HRIR has correct delay slot wiring", "[audio][hrtf]") {
    using namespace psynder::audio::detail;
    const StereoHrir h = make_minimal_hrir(+psynder::math::kHalfPi, /*sr*/ 48000u);
    // right is ipsilateral → no delay; left is contralateral → delayed
    REQUIRE(h.right_delay_samples == 0u);
    REQUIRE(h.left_delay_samples  >= 30u);
    REQUIRE(h.left_delay_samples  <= 34u);
}

// ─── 2-D HRTF (azimuth + elevation) ────────────────────────────────────
// These tests cover the Brown-Duda structural model extension that adds
// elevation. The cheap analytical form (no measured HRIR) is enough to
// hit the expected invariants: median-plane sources have ~zero ITD,
// pinna comb taps depend on elevation, distance attenuation is 1/r.

TEST_CASE("audio: 2-D HRTF - median plane has zero ITD", "[audio][hrtf]") {
    using namespace psynder::audio::detail;
    // Source directly above the head → median plane.
    psynder::f32 itd = itd_seconds_2d(/*az=*/0.0f, /*el=*/+psynder::math::kHalfPi);
    REQUIRE(std::fabs(itd) < 1e-6f);
    // Source directly below the head → still median plane.
    itd = itd_seconds_2d(/*az=*/0.0f, /*el=*/-psynder::math::kHalfPi);
    REQUIRE(std::fabs(itd) < 1e-6f);
    // Source on the median plane at any elevation has zero ITD.
    for (psynder::f32 el : {-0.5f, -0.2f, 0.0f, 0.3f, 1.0f}) {
        itd = itd_seconds_2d(0.0f, el);
        INFO("elevation=" << el);
        REQUIRE(std::fabs(itd) < 1e-6f);
    }
}

TEST_CASE("audio: 2-D HRTF - ITD shrinks as source elevates", "[audio][hrtf]") {
    using namespace psynder::audio::detail;
    // Lateral source at +π/2 azimuth: horizontal-plane ITD is maximum.
    const psynder::f32 horiz = itd_seconds_2d(+psynder::math::kHalfPi, 0.0f);
    const psynder::f32 mid   = itd_seconds_2d(+psynder::math::kHalfPi,
                                              psynder::math::kHalfPi * 0.5f);
    const psynder::f32 up    = itd_seconds_2d(+psynder::math::kHalfPi,
                                              psynder::math::kHalfPi * 0.95f);
    INFO("horiz=" << horiz << " mid=" << mid << " up=" << up);
    REQUIRE(horiz > mid);
    REQUIRE(mid   > up);
    REQUIRE(up    >= 0.0f);
    // Above-listener has ITD ≈ 0 by construction.
    REQUIRE(std::fabs(itd_seconds_2d(psynder::math::kHalfPi,
                                     psynder::math::kHalfPi)) < 1e-6f);
}

TEST_CASE("audio: 2-D HRIR carries distance attenuation", "[audio][hrtf]") {
    using namespace psynder::audio::detail;
    // 1 m source should have gain 1.0 (clamped); 10 m should be 0.1.
    const StereoHrir h_near = make_hrir_2d(+0.5f, 0.0f, /*r=*/1.0f, 48000u);
    const StereoHrir h_far  = make_hrir_2d(+0.5f, 0.0f, /*r=*/10.0f, 48000u);
    REQUIRE(std::fabs(h_near.left_gain - 1.0f) < 1e-6f);
    REQUIRE(std::fabs(h_far.left_gain  - 0.1f) < 1e-5f);
}

TEST_CASE("audio: apply_hrtf produces non-trivial stereo with elevation",
          "[audio][hrtf]") {
    using namespace psynder::audio::detail;
    constexpr psynder::u32 N = 256;
    std::vector<psynder::f32> mono_in(N, 0.0f);
    // Impulse at sample 8.
    mono_in[8] = 1.0f;
    std::vector<psynder::f32> left_horiz(N, 0.0f),  right_horiz(N, 0.0f);
    std::vector<psynder::f32> left_up(N, 0.0f),     right_up(N, 0.0f);
    // Source on the right at horizon vs at +45° elevation.
    apply_hrtf(left_horiz.data(), right_horiz.data(), mono_in.data(), N,
               +psynder::math::kHalfPi * 0.5f, 0.0f, 1.0f, 48000u);
    apply_hrtf(left_up.data(),    right_up.data(),    mono_in.data(), N,
               +psynder::math::kHalfPi * 0.5f,
               +psynder::math::kHalfPi * 0.5f, 1.0f, 48000u);
    // Both outputs must be non-zero.
    psynder::f32 energy_horiz = 0.0f, energy_up = 0.0f;
    for (psynder::u32 i = 0; i < N; ++i) {
        energy_horiz += left_horiz[i]*left_horiz[i] + right_horiz[i]*right_horiz[i];
        energy_up    += left_up[i]*left_up[i]       + right_up[i]*right_up[i];
    }
    INFO("energy_horiz=" << energy_horiz << " energy_up=" << energy_up);
    REQUIRE(energy_horiz > 0.0f);
    REQUIRE(energy_up    > 0.0f);
    // The two should DIFFER — the elevation pinna comb changes the
    // produced impulse signature. We don't assert magnitudes; just that
    // the impulse responses are not identical.
    psynder::f32 diff = 0.0f;
    for (psynder::u32 i = 0; i < N; ++i) {
        diff += std::fabs(left_horiz[i] - left_up[i]) +
                std::fabs(right_horiz[i] - right_up[i]);
    }
    REQUIRE(diff > 1e-4f);
}
