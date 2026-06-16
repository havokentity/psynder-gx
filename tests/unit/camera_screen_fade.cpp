// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/camera_screen_fade.cpp — full-screen fade envelope (to/from black
// or white, with a hold). See engine/camera/ScreenFade.h.

#include "camera/ScreenFade.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>    // std::abs
#include <limits>

using namespace psynder;
using namespace psynder::camera;

namespace {

// A simple authored envelope: 0.5 s in, 1.0 s hold, 0.5 s out, full black.
FadeParams make_params() {
    FadeParams p;
    p.fade_in_s  = 0.5f;
    p.hold_s     = 1.0f;
    p.fade_out_s = 0.5f;
    p.peak_alpha = 1.0f;
    return p;
}

}  // namespace

TEST_CASE("screen fade: a fresh init is idle and fully clear", "[camera]") {
    FadeState s;
    fade_init(s);
    CHECK(s.phase == kFadePhaseIdle);
    CHECK_FALSE(fade_active(s));
    CHECK(fade_alpha(s) == Catch::Approx(0.0f));
}

TEST_CASE("screen fade: start then update ramps alpha up to peak over the fade in", "[camera]") {
    const FadeParams p = make_params();
    FadeState s;
    fade_init(s);
    fade_start(s, p);

    CHECK(fade_active(s));
    CHECK(s.phase == kFadePhaseIn);
    CHECK(fade_alpha(s) == Catch::Approx(0.0f));   // nothing advanced yet

    // Quarter of the fade-in: 0.125 / 0.5 == 0.25 of peak.
    fade_update(s, p, 0.125f);
    CHECK(fade_alpha(s) == Catch::Approx(0.25f));
    CHECK(s.phase == kFadePhaseIn);

    // Halfway through the fade-in: 0.25 / 0.5 == 0.5 of peak.
    fade_update(s, p, 0.125f);
    CHECK(fade_alpha(s) == Catch::Approx(0.5f));
    CHECK(s.phase == kFadePhaseIn);

    // Finish the fade-in exactly: alpha reaches peak and the phase rolls to hold.
    fade_update(s, p, 0.25f);
    CHECK(fade_alpha(s) == Catch::Approx(1.0f));
    CHECK(s.phase == kFadePhaseHold);
    CHECK(fade_active(s));
}

TEST_CASE("screen fade: it holds at peak through the whole hold span", "[camera]") {
    const FadeParams p = make_params();
    FadeState s;
    fade_init(s);
    fade_start(s, p);

    fade_update(s, p, 0.5f);                  // complete the fade-in
    CHECK(s.phase == kFadePhaseHold);
    CHECK(fade_alpha(s) == Catch::Approx(1.0f));

    // Partway through the hold: still pinned at peak.
    fade_update(s, p, 0.4f);
    CHECK(s.phase == kFadePhaseHold);
    CHECK(fade_alpha(s) == Catch::Approx(1.0f));

    // Most of the rest of the hold (total 0.9 of 1.0): still pinned at peak.
    fade_update(s, p, 0.5f);
    CHECK(s.phase == kFadePhaseHold);
    CHECK(fade_alpha(s) == Catch::Approx(1.0f));
}

TEST_CASE("screen fade: it ramps back to zero over the fade out and returns to idle", "[camera]") {
    const FadeParams p = make_params();
    FadeState s;
    fade_init(s);
    fade_start(s, p);

    fade_update(s, p, 0.5f);   // fade-in done
    fade_update(s, p, 1.0f);   // hold done -> now at the start of fade-out
    CHECK(s.phase == kFadePhaseOut);
    CHECK(fade_alpha(s) == Catch::Approx(1.0f));   // still at peak entering out

    // Halfway through the fade-out: 0.25 / 0.5 == 0.5 -> alpha 0.5.
    fade_update(s, p, 0.25f);
    CHECK(s.phase == kFadePhaseOut);
    CHECK(fade_alpha(s) == Catch::Approx(0.5f));

    // Finish the fade-out: alpha hits 0 and the envelope goes idle / inactive.
    fade_update(s, p, 0.25f);
    CHECK(s.phase == kFadePhaseIdle);
    CHECK_FALSE(fade_active(s));
    CHECK(fade_alpha(s) == Catch::Approx(0.0f));
}

TEST_CASE("screen fade: a big dt that overshoots a phase carries the remainder forward", "[camera]") {
    const FadeParams p = make_params();   // 0.5 in, 1.0 hold, 0.5 out
    FadeState s;
    fade_init(s);
    fade_start(s, p);

    // One huge step (0.5 + 1.0 + 0.25 = 1.75 s) lands the envelope halfway
    // through the fade-OUT: the remainder spills across the in and hold phases.
    fade_update(s, p, 1.75f);
    CHECK(s.phase == kFadePhaseOut);
    CHECK(fade_alpha(s) == Catch::Approx(0.5f));   // 0.25 of 0.5 s into the out

    // A single step larger than the entire remaining envelope finishes it and
    // discards the leftover — alpha 0, idle.
    fade_update(s, p, 10.0f);
    CHECK(s.phase == kFadePhaseIdle);
    CHECK_FALSE(fade_active(s));
    CHECK(fade_alpha(s) == Catch::Approx(0.0f));
}

TEST_CASE("screen fade: alpha never exceeds peak alpha", "[camera]") {
    FadeParams p = make_params();
    p.peak_alpha = 0.6f;        // a translucent tint, not full black
    FadeState s;
    fade_init(s);
    fade_start(s, p);

    // Step through the whole envelope in small increments; alpha must stay in
    // [0, peak] at every sample and peak at exactly peak_alpha during the hold.
    f32 max_seen = 0.0f;
    for (int i = 0; i < 60; ++i) {
        fade_update(s, p, 0.05f);
        const f32 a = fade_alpha(s);
        CHECK(a >= 0.0f);
        CHECK(a <= Catch::Approx(0.6f));
        if (a > max_seen) max_seen = a;
    }
    CHECK(max_seen == Catch::Approx(0.6f));   // the hold reached the peak
}

TEST_CASE("screen fade: a zero fade in jumps straight to peak and holds", "[camera]") {
    FadeParams p = make_params();
    p.fade_in_s = 0.0f;         // instant black-out
    FadeState s;
    fade_init(s);
    fade_start(s, p);

    // No visible ramp-in: already at peak, already in the hold phase.
    CHECK(s.phase == kFadePhaseHold);
    CHECK(fade_alpha(s) == Catch::Approx(1.0f));
    CHECK(fade_active(s));
}

TEST_CASE("screen fade: a zero or non-finite dt leaves the envelope untouched", "[camera]") {
    const FadeParams p = make_params();
    FadeState s;
    fade_init(s);
    fade_start(s, p);

    fade_update(s, p, 0.25f);                 // advance halfway into the fade-in
    const f32 a_before = fade_alpha(s);
    const f32 t_before = s.timer_s;
    const u32 ph_before = s.phase;
    CHECK(a_before == Catch::Approx(0.5f));

    fade_update(s, p, 0.0f);                  // zero dt -> no advance
    CHECK(fade_alpha(s) == a_before);
    CHECK(s.timer_s == t_before);
    CHECK(s.phase == ph_before);

    fade_update(s, p, -1.0f);                 // negative dt -> no advance
    CHECK(fade_alpha(s) == a_before);
    CHECK(s.timer_s == t_before);

    const f32 inf = std::numeric_limits<f32>::infinity();
    fade_update(s, p, inf);                   // non-finite dt -> no advance
    CHECK(fade_alpha(s) == a_before);
    CHECK(s.timer_s == t_before);

    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    fade_update(s, p, nan);                   // non-finite dt -> no advance
    CHECK(fade_alpha(s) == a_before);
    CHECK(s.timer_s == t_before);

    CHECK(fade_active(s));                     // still mid-fade, untouched
}

TEST_CASE("screen fade: updating an idle envelope is a no-op", "[camera]") {
    const FadeParams p = make_params();
    FadeState s;
    fade_init(s);

    fade_update(s, p, 0.5f);                   // nothing started — must stay idle
    CHECK(s.phase == kFadePhaseIdle);
    CHECK_FALSE(fade_active(s));
    CHECK(fade_alpha(s) == Catch::Approx(0.0f));
}

TEST_CASE("screen fade: the envelope is deterministic", "[camera][determinism]") {
    const FadeParams p = make_params();
    FadeState a;
    FadeState b;
    fade_init(a);
    fade_init(b);
    fade_start(a, p);
    fade_start(b, p);

    for (int i = 0; i < 120; ++i) {
        fade_update(a, p, 1.0f / 60.0f);
        fade_update(b, p, 1.0f / 60.0f);
        // Bit-identical after identical input sequences.
        CHECK(fade_alpha(a) == fade_alpha(b));
        CHECK(a.timer_s == b.timer_s);
        CHECK(a.phase == b.phase);
    }
    CHECK(a.alpha == b.alpha);
    CHECK(a.timer_s == b.timer_s);
    CHECK(a.phase == b.phase);
}
