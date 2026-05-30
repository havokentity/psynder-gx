// SPDX-License-Identifier: MIT
// Psynder — lane 14 unit test: selectable distance-attenuation rolloff models
// (Attenuation.h), the SELECTABLE rolloff family complementing SpatialCue.h's
// single fixed inverse-distance curve.
//
// Verifies the load-bearing properties of engine/audio/Attenuation.{h,cpp}:
//   (a) EVERY model is exactly 1.0 at/under ref and 0.0 at/beyond max, and is
//       monotonically decreasing strictly between (and stays in [0,1]);
//   (b) Linear hits a known value 0.5 at the band midpoint;
//   (c) InverseSquare falls off FASTER than Linear at the same mid-distance
//       (the curve ordering sound designers rely on);
//   (d) attenuation_db is ~0 dB at full gain and the large negative floor at 0;
//   (e) the degenerate range (max <= ref) collapses to a clean 1/0 step;
//   (f) determinism: two identical calls are bit-identical.

#include "audio/Attenuation.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace psynder;
using namespace psynder::audio;

namespace {

// The four selectable models, iterated so the shared contract is asserted for
// each without repeating the body.
constexpr RolloffModel kModels[] = {
    RolloffModel::Linear,
    RolloffModel::Inverse,
    RolloffModel::InverseSquare,
    RolloffModel::Exponential,
};

}  // namespace

TEST_CASE("audio: every rolloff model is unity within ref and silent past max", "[audio][attenuation]") {
    const f32 ref = 2.0f, max = 50.0f, f = 1.0f;

    for (const RolloffModel m : kModels) {
        INFO("model=" << static_cast<unsigned>(m));

        // At and under the reference radius: full gain.
        REQUIRE(attenuation(m, 0.0f, ref, max, f) == Catch::Approx(1.0f).margin(1e-6f));
        REQUIRE(attenuation(m, ref,  ref, max, f) == Catch::Approx(1.0f).margin(1e-6f));

        // At and beyond the max radius: silent (hard cutoff on every model).
        REQUIRE(attenuation(m, max,          ref, max, f) == Catch::Approx(0.0f).margin(1e-6f));
        REQUIRE(attenuation(m, max + 25.0f,  ref, max, f) == Catch::Approx(0.0f).margin(1e-6f));

        // Every sample across the whole range stays in [0,1].
        for (f32 d = 0.0f; d <= 70.0f; d += 3.0f) {
            const f32 g = attenuation(m, d, ref, max, f);
            REQUIRE(g >= 0.0f);
            REQUIRE(g <= 1.0f);
        }
    }
}

TEST_CASE("audio: every rolloff model decreases monotonically across the band", "[audio][attenuation]") {
    const f32 ref = 2.0f, max = 50.0f, f = 1.0f;

    for (const RolloffModel m : kModels) {
        INFO("model=" << static_cast<unsigned>(m));

        const f32 g_near = attenuation(m, 6.0f,  ref, max, f);
        const f32 g_mid  = attenuation(m, 20.0f, ref, max, f);
        const f32 g_far  = attenuation(m, 40.0f, ref, max, f);
        INFO("g_near=" << g_near << " g_mid=" << g_mid << " g_far=" << g_far);

        REQUIRE(g_near < 1.0f);
        REQUIRE(g_mid  < g_near);
        REQUIRE(g_far  < g_mid);
        REQUIRE(g_far  > 0.0f);
    }
}

TEST_CASE("audio: linear rolloff is one half at the band midpoint", "[audio][attenuation]") {
    const f32 ref = 0.0f, max = 100.0f, f = 1.0f;

    // Midpoint of [0,100] is 50 m => 1 - 50/100 == 0.5 exactly.
    REQUIRE(attenuation(RolloffModel::Linear, 50.0f, ref, max, f)
            == Catch::Approx(0.5f).margin(1e-6f));

    // And the quarter / three-quarter points for good measure.
    REQUIRE(attenuation(RolloffModel::Linear, 25.0f, ref, max, f)
            == Catch::Approx(0.75f).margin(1e-6f));
    REQUIRE(attenuation(RolloffModel::Linear, 75.0f, ref, max, f)
            == Catch::Approx(0.25f).margin(1e-6f));
}

TEST_CASE("audio: inverse square falls off faster than linear at the same distance", "[audio][attenuation]") {
    const f32 ref = 1.0f, max = 100.0f, f = 1.0f;
    const f32 d = 20.0f;  // a mid-band distance well clear of both endpoints

    const f32 g_linear = attenuation(RolloffModel::Linear,        d, ref, max, f);
    const f32 g_invsq   = attenuation(RolloffModel::InverseSquare, d, ref, max, f);
    INFO("g_linear=" << g_linear << " g_invsq=" << g_invsq);

    // The whole point of offering inverse-square: it is quieter (steeper) than
    // the straight line at a shared mid-distance.
    REQUIRE(g_invsq < g_linear);

    // Inverse (1/x) sits between the linear floor and the inverse-square — also
    // steeper than linear at the same point.
    const f32 g_inverse = attenuation(RolloffModel::Inverse, d, ref, max, f);
    INFO("g_inverse=" << g_inverse);
    REQUIRE(g_inverse < g_linear);
    REQUIRE(g_invsq   < g_inverse);
}

TEST_CASE("audio: attenuation_db is zero at full gain and floored at silence", "[audio][attenuation]") {
    const f32 ref = 2.0f, max = 50.0f, f = 1.0f;

    for (const RolloffModel m : kModels) {
        INFO("model=" << static_cast<unsigned>(m));

        // Full gain (inside ref) => 0 dB.
        REQUIRE(attenuation_db(m, 0.0f, ref, max, f) == Catch::Approx(0.0f).margin(1e-4f));

        // Silent (at/beyond max) => the large negative floor, never -infinity.
        const f32 db_silent = attenuation_db(m, max, ref, max, f);
        REQUIRE(db_silent == Catch::Approx(kAttenuationDbFloor).margin(1e-4f));
        REQUIRE(std::isfinite(db_silent));

        // A mid-band level is between the floor and 0 dB.
        const f32 db_mid = attenuation_db(m, 20.0f, ref, max, f);
        REQUIRE(db_mid < 0.0f);
        REQUIRE(db_mid > kAttenuationDbFloor);
    }

    // -6 dB is half-amplitude: linear at its midpoint (gain 0.5) lands there.
    const f32 db_half = attenuation_db(RolloffModel::Linear, 50.0f, 0.0f, 100.0f, 1.0f);
    REQUIRE(db_half == Catch::Approx(20.0f * std::log10(0.5f)).margin(1e-4f));
}

TEST_CASE("audio: a degenerate range collapses to a clean step", "[audio][attenuation]") {
    // max <= ref leaves no band: full gain at/under ref, silence beyond.
    const f32 ref = 10.0f, max = 5.0f, f = 1.0f;

    for (const RolloffModel m : kModels) {
        INFO("model=" << static_cast<unsigned>(m));
        REQUIRE(attenuation(m, 0.0f,  ref, max, f) == Catch::Approx(1.0f).margin(1e-6f));
        REQUIRE(attenuation(m, ref,   ref, max, f) == Catch::Approx(1.0f).margin(1e-6f));
        REQUIRE(attenuation(m, ref + 0.001f, ref, max, f) == Catch::Approx(0.0f).margin(1e-6f));
        REQUIRE(attenuation(m, 100.0f, ref, max, f) == Catch::Approx(0.0f).margin(1e-6f));
    }
}

TEST_CASE("audio: attenuation models are deterministic across identical calls", "[audio][attenuation]") {
    const f32 ref = 1.5f, max = 80.0f, f = 1.25f;

    for (const RolloffModel m : kModels) {
        INFO("model=" << static_cast<unsigned>(m));
        // Same inputs => bit-identical outputs (same-platform determinism).
        REQUIRE(attenuation(m, 33.0f, ref, max, f)    == attenuation(m, 33.0f, ref, max, f));
        REQUIRE(attenuation_db(m, 33.0f, ref, max, f) == attenuation_db(m, 33.0f, ref, max, f));
    }
}
