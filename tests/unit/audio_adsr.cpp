// SPDX-License-Identifier: MIT
//
// tests/unit/audio_adsr.cpp — the ADSR amplitude envelope.

#include "audio/Adsr.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace psynder;
using namespace psynder::audio;

namespace {
// attack 0.1, decay 0.2, sustain 0.5, release 0.3.
const AdsrParams kP{0.1f, 0.2f, 0.5f, 0.3f};
}  // namespace

TEST_CASE("adsr: attack ramps from zero to one", "[audio]") {
    CHECK(adsr_gain(kP, 0.0f, false, 0.0f) == Catch::Approx(0.0f));
    CHECK(adsr_gain(kP, 0.05f, false, 0.0f) == Catch::Approx(0.5f));  // halfway up
    CHECK(adsr_gain(kP, 0.1f, false, 0.0f) == Catch::Approx(1.0f));   // peak
}

TEST_CASE("adsr: decay falls from one to sustain then holds", "[audio]") {
    // Mid-decay (0.1 attack + 0.1 of 0.2 decay): 1 - 0.5*(0.1/0.2) = 0.75.
    CHECK(adsr_gain(kP, 0.2f, false, 0.0f) == Catch::Approx(0.75f));
    // End of decay -> sustain.
    CHECK(adsr_gain(kP, 0.3f, false, 0.0f) == Catch::Approx(0.5f));
    // Held well past attack+decay -> still sustain.
    CHECK(adsr_gain(kP, 2.0f, false, 0.0f) == Catch::Approx(0.5f));
}

TEST_CASE("adsr: release ramps from sustain to zero", "[audio]") {
    CHECK(adsr_gain(kP, 1.0f, true, 0.0f) == Catch::Approx(0.5f));    // at release start
    CHECK(adsr_gain(kP, 1.0f, true, 0.15f) == Catch::Approx(0.25f));  // halfway down
    CHECK(adsr_gain(kP, 1.0f, true, 0.3f) == Catch::Approx(0.0f));    // end of release
    CHECK(adsr_gain(kP, 1.0f, true, 1.0f) == Catch::Approx(0.0f));    // past release
}

TEST_CASE("adsr: zero-length stages jump instantly", "[audio]") {
    const AdsrParams instant{0.0f, 0.0f, 0.4f, 0.0f};
    CHECK(adsr_gain(instant, 0.0f, false, 0.0f) == Catch::Approx(1.0f));   // no attack
    CHECK(adsr_gain(instant, 0.001f, false, 0.0f) == Catch::Approx(0.4f)); // no decay
    CHECK(adsr_gain(instant, 0.0f, true, 0.0f) == Catch::Approx(0.0f));    // no release
}

TEST_CASE("adsr: the result clamps to the unit interval", "[audio]") {
    const AdsrParams hot{0.1f, 0.1f, 2.0f, 0.1f};  // sustain > 1 clamps
    const f32 g = adsr_gain(hot, 5.0f, false, 0.0f);
    CHECK(g <= 1.0f);
    CHECK(g >= 0.0f);
    CHECK(adsr_gain(kP, -1.0f, false, 0.0f) == Catch::Approx(0.0f));  // negative t
}

TEST_CASE("adsr: the voice helper tracks the same curve and finishes", "[audio]") {
    AdsrVoice v;
    adsr_note_on(v);
    CHECK(adsr_voice_gain(v, kP) == Catch::Approx(0.0f));
    adsr_advance(v, 0.1f);  // end of attack
    CHECK(adsr_voice_gain(v, kP) == Catch::Approx(1.0f));
    adsr_advance(v, 0.2f);  // end of decay
    CHECK(adsr_voice_gain(v, kP) == Catch::Approx(0.5f));
    CHECK_FALSE(adsr_finished(v, kP));

    adsr_note_off(v);
    CHECK(adsr_voice_gain(v, kP) == Catch::Approx(0.5f));  // release starts at sustain
    adsr_advance(v, 0.15f);
    CHECK(adsr_voice_gain(v, kP) == Catch::Approx(0.25f));
    CHECK_FALSE(adsr_finished(v, kP));
    adsr_advance(v, 0.2f);  // total 0.35 > 0.3 release
    CHECK(adsr_voice_gain(v, kP) == Catch::Approx(0.0f));
    CHECK(adsr_finished(v, kP));
}

TEST_CASE("adsr: gain is deterministic", "[audio][determinism]") {
    for (int i = 0; i < 16; ++i) {
        CHECK(adsr_gain(kP, 0.18f, false, 0.0f) == adsr_gain(kP, 0.18f, false, 0.0f));
        CHECK(adsr_gain(kP, 1.0f, true, 0.12f) == adsr_gain(kP, 1.0f, true, 0.12f));
    }
}
