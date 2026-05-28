// SPDX-License-Identifier: MIT
// Psynder — lane 14 unit test: positional HDR audio mix math (ADR-023 / #47).
//
// Verifies the three load-bearing properties of engine/audio/PositionalMix.h:
//   (a) occlusion makes a source both quieter AND duller (lower attenuation
//       and lower cutoff than an unoccluded source);
//   (b) panning is constant-power and steers the correct way (hard-left gives
//       left>right, hard-right gives right>left, L^2 + R^2 == 1);
//   (c) HDR gains keep the in-phase summed peak at/under full scale (no clip),
//       even for a loud + quiet mix.

#include "audio/PositionalMix.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <span>

using psynder::f32;
namespace audio = psynder::audio;

TEST_CASE("audio: occlusion lowers both attenuation and cutoff", "[audio][positional]") {
    f32 atten_open = 0.0f, cutoff_open = 0.0f;
    f32 atten_sealed = 0.0f, cutoff_sealed = 0.0f;

    audio::occlusion_filter(0.0f, atten_open, cutoff_open);
    audio::occlusion_filter(1.0f, atten_sealed, cutoff_sealed);

    INFO("open atten=" << atten_open << " cutoff=" << cutoff_open
         << " | sealed atten=" << atten_sealed << " cutoff=" << cutoff_sealed);

    // Fully occluded => quieter (lower linear attenuation).
    REQUIRE(atten_sealed < atten_open);
    // Fully occluded => duller (lower low-pass cutoff).
    REQUIRE(cutoff_sealed < cutoff_open);

    // Open should be effectively unattenuated and full-band.
    REQUIRE(atten_open == Catch::Approx(1.0f).margin(1e-5f));
    REQUIRE(cutoff_open == Catch::Approx(audio::kOcclusionMaxCutoffHz).margin(1e-3f));
}

TEST_CASE("audio: pan_lr is directional and constant power", "[audio][positional]") {
    using psynder::math::kHalfPi;

    f32 l = 0.0f, r = 0.0f;

    // Hard left: more left gain than right.
    audio::pan_lr(-kHalfPi, l, r);
    INFO("hard-left L=" << l << " R=" << r);
    REQUIRE(l > r);
    REQUIRE((l * l + r * r) == Catch::Approx(1.0f).margin(1e-5f));

    // Hard right: more right gain than left.
    audio::pan_lr(+kHalfPi, l, r);
    INFO("hard-right L=" << l << " R=" << r);
    REQUIRE(r > l);
    REQUIRE((l * l + r * r) == Catch::Approx(1.0f).margin(1e-5f));

    // Centre: equal and still constant power.
    audio::pan_lr(0.0f, l, r);
    INFO("centre L=" << l << " R=" << r);
    REQUIRE(l == Catch::Approx(r).margin(1e-5f));
    REQUIRE((l * l + r * r) == Catch::Approx(1.0f).margin(1e-5f));
}

TEST_CASE("audio: hdr_gains never lets the summed peak clip", "[audio][positional]") {
    // A loud source (0 dBFS, already at full scale) plus a quieter one.
    const std::array<f32, 3> loudness_db{ 0.0f, -6.0f, -12.0f };
    std::array<f32, 3> gains{};

    const f32 headroom_db = 1.0f;
    audio::hdr_gains(std::span<const f32>(loudness_db),
                     std::span<f32>(gains),
                     headroom_db);

    // Worst-case (in-phase) summed amplitude after gain must stay <= full
    // scale, accounting for the requested headroom ceiling.
    f32 summed = 0.0f;
    for (std::size_t i = 0; i < loudness_db.size(); ++i) {
        const f32 amp = std::pow(10.0f, loudness_db[i] / 20.0f);
        summed += gains[i] * amp;
        INFO("src " << i << " gain=" << gains[i]);
    }
    INFO("summed in-phase peak=" << summed);

    const f32 ceiling = std::pow(10.0f, -headroom_db / 20.0f);
    REQUIRE(summed <= Catch::Approx(ceiling).margin(1e-5f));
    REQUIRE(summed <= Catch::Approx(1.0f).margin(1e-5f));  // never clips

    // Every gain stays in [0,1].
    for (f32 g : gains) {
        REQUIRE(g >= 0.0f);
        REQUIRE(g <= 1.0f);
    }
}
