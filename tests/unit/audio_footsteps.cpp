// SPDX-License-Identifier: MIT
// Psynder — lane 14 unit test: distance-driven footstep cadence (Footsteps.h).
//
// Verifies the load-bearing properties of engine/audio/Footsteps.h:
//   (a) standing still (zero travel) never emits a step;
//   (b) moving exactly one stride fires exactly one step;
//   (c) several small moves accumulate, fire a step when the stride is reached,
//       and carry the leftover remainder forward (no drift);
//   (d) a single big move covering N strides fires N steps in one call
//       (step_count advances by N) per the documented looping multi-stride rule;
//   (e) faster movement steps more often than slower movement over the same
//       number of equal-dt calls;
//   (f) a non-positive stride is guarded: no step, state untouched;
//   (g) reset/init zero both fields;
//   (h) footstep_progress stays in [0,1) and tracks the banked distance;
//   (i) determinism: identical input sequences => identical step patterns.

#include "audio/Footsteps.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace psynder;
using namespace psynder::audio;

TEST_CASE("audio: standing still never emits a footstep", "[audio][footsteps]") {
    FootstepState s;
    footsteps_init(s);

    // Zero travel, many calls: no step ever fires, nothing accumulates.
    for (int i = 0; i < 100; ++i) {
        REQUIRE(footstep_advance(s, 0.0f, 1.5f) == false);
    }
    REQUIRE(s.step_count == 0u);
    REQUIRE(s.distance_accum_m == Catch::Approx(0.0f).margin(1e-6f));
}

TEST_CASE("audio: moving exactly one stride fires exactly one step", "[audio][footsteps]") {
    FootstepState s;
    footsteps_init(s);

    const f32 stride = 0.75f;
    REQUIRE(footstep_advance(s, stride, stride) == true);
    REQUIRE(s.step_count == 1u);

    // After consuming exactly one stride the bank is empty again.
    REQUIRE(s.distance_accum_m == Catch::Approx(0.0f).margin(1e-6f));
}

TEST_CASE("audio: small moves accumulate and carry the remainder forward", "[audio][footsteps]") {
    FootstepState s;
    footsteps_init(s);

    const f32 stride = 1.0f;

    // Four 0.3 m nudges bank 1.2 m: the step fires only when the bank crosses
    // the 1.0 m stride (on the fourth nudge), not before.
    REQUIRE(footstep_advance(s, 0.3f, stride) == false);  // 0.3
    REQUIRE(footstep_advance(s, 0.3f, stride) == false);  // 0.6
    REQUIRE(footstep_advance(s, 0.3f, stride) == false);  // 0.9
    REQUIRE(footstep_advance(s, 0.3f, stride) == true);   // 1.2 -> step, 0.2 carried
    REQUIRE(s.step_count == 1u);

    // The 0.2 m remainder carried forward, so the next step needs only 0.8 m
    // more (0.2 + 0.8 = 1.0). It should NOT fire after just 0.7 m, but SHOULD
    // after another 0.1 m.
    REQUIRE(s.distance_accum_m == Catch::Approx(0.2f).margin(1e-5f));
    REQUIRE(footstep_advance(s, 0.7f, stride) == false);  // 0.9
    REQUIRE(footstep_advance(s, 0.1f, stride) == true);   // 1.0 -> step
    REQUIRE(s.step_count == 2u);
}

TEST_CASE("audio: a big move covering N strides fires N steps in one call", "[audio][footsteps]") {
    FootstepState s;
    footsteps_init(s);

    const f32 stride = 0.5f;

    // A single 2.6 m move (a frame spike / catch-up) covers five whole 0.5 m
    // strides (2.5 m) with 0.1 m left over. The looping rule emits all five.
    REQUIRE(footstep_advance(s, 2.6f, stride) == true);
    REQUIRE(s.step_count == 5u);
    REQUIRE(s.distance_accum_m == Catch::Approx(0.1f).margin(1e-5f));

    // Counting steps fired in a single call: diff step_count across the call.
    const u32 before = s.step_count;
    footstep_advance(s, 1.5f, stride);  // 0.1 + 1.5 = 1.6 -> three strides
    REQUIRE(s.step_count - before == 3u);
}

TEST_CASE("audio: faster movement steps more often than slower over equal calls", "[audio][footsteps]") {
    const f32 stride = 1.0f;

    FootstepState slow;
    FootstepState fast;
    footsteps_init(slow);
    footsteps_init(fast);

    // Same number of equal-dt calls; the fast pawn covers more ground per call.
    const int calls = 20;
    for (int i = 0; i < calls; ++i) {
        footstep_advance(slow, 0.20f, stride);  // slow walk
        footstep_advance(fast, 0.60f, stride);  // sprint
    }

    INFO("slow steps=" << slow.step_count << " fast steps=" << fast.step_count);
    REQUIRE(fast.step_count > slow.step_count);

    // Sanity: 20 * 0.20 = 4.0 m => 4 steps; 20 * 0.60 = 12.0 m => 12 steps.
    REQUIRE(slow.step_count == 4u);
    REQUIRE(fast.step_count == 12u);
}

TEST_CASE("audio: negative travel is clamped and never rewinds the cadence", "[audio][footsteps]") {
    FootstepState s;
    footsteps_init(s);

    const f32 stride = 1.0f;

    // Bank some distance, then feed a negative move: it must be treated as 0,
    // leaving the bank exactly where it was (no un-walking).
    REQUIRE(footstep_advance(s, 0.4f, stride) == false);
    REQUIRE(footstep_advance(s, -5.0f, stride) == false);
    REQUIRE(s.distance_accum_m == Catch::Approx(0.4f).margin(1e-5f));
    REQUIRE(s.step_count == 0u);
}

TEST_CASE("audio: a non-positive stride is guarded and leaves state untouched", "[audio][footsteps]") {
    FootstepState s;
    footsteps_init(s);
    s.distance_accum_m = 0.42f;
    s.step_count       = 7u;

    // Zero and negative strides: no step, and the state is completely unchanged
    // (no accumulation, no count change).
    REQUIRE(footstep_advance(s, 10.0f, 0.0f) == false);
    REQUIRE(s.distance_accum_m == Catch::Approx(0.42f).margin(1e-6f));
    REQUIRE(s.step_count == 7u);

    REQUIRE(footstep_advance(s, 10.0f, -2.0f) == false);
    REQUIRE(s.distance_accum_m == Catch::Approx(0.42f).margin(1e-6f));
    REQUIRE(s.step_count == 7u);

    // Progress is also guarded to 0 for a non-positive stride.
    REQUIRE(footstep_progress(s, 0.0f) == Catch::Approx(0.0f).margin(1e-6f));
    REQUIRE(footstep_progress(s, -1.0f) == Catch::Approx(0.0f).margin(1e-6f));
}

TEST_CASE("audio: reset and init zero both fields", "[audio][footsteps]") {
    FootstepState s;
    s.distance_accum_m = 3.14f;
    s.step_count       = 99u;

    footsteps_reset(s);
    REQUIRE(s.distance_accum_m == Catch::Approx(0.0f).margin(1e-6f));
    REQUIRE(s.step_count == 0u);

    // Dirty it again and confirm init is equivalent.
    s.distance_accum_m = 2.71f;
    s.step_count       = 5u;
    footsteps_init(s);
    REQUIRE(s.distance_accum_m == Catch::Approx(0.0f).margin(1e-6f));
    REQUIRE(s.step_count == 0u);
}

TEST_CASE("audio: footstep_progress stays in 0 to 1 and tracks the bank", "[audio][footsteps]") {
    FootstepState s;
    footsteps_init(s);

    const f32 stride = 2.0f;

    // Fresh: no progress.
    REQUIRE(footstep_progress(s, stride) == Catch::Approx(0.0f).margin(1e-6f));

    // Half a stride banked => progress ~0.5, strictly inside [0,1).
    footstep_advance(s, 1.0f, stride);  // 1.0 of 2.0
    const f32 p = footstep_progress(s, stride);
    INFO("progress=" << p);
    REQUIRE(p == Catch::Approx(0.5f).margin(1e-5f));
    REQUIRE(p >= 0.0f);
    REQUIRE(p < 1.0f);

    // Sweep a range of partial banks; progress never reaches or exceeds 1.0
    // because completed strides are always subtracted off by advance().
    for (int i = 0; i < 50; ++i) {
        footstep_advance(s, 0.137f, stride);
        const f32 pr = footstep_progress(s, stride);
        REQUIRE(pr >= 0.0f);
        REQUIRE(pr < 1.0f);
    }
}

TEST_CASE("audio: identical input sequences give identical step patterns", "[audio][footsteps]") {
    const f32 stride = 0.85f;

    // Two states driven by the same (deterministic, irregular) move sequence
    // must end bit-identical and fire on exactly the same calls.
    const f32 moves[] = {0.1f, 0.3f, 0.0f, 0.9f, 0.25f, 1.7f, 0.05f, 0.5f, 0.5f, 0.5f};

    FootstepState a;
    FootstepState b;
    footsteps_init(a);
    footsteps_init(b);

    for (const f32 m : moves) {
        const bool fa = footstep_advance(a, m, stride);
        const bool fb = footstep_advance(b, m, stride);
        REQUIRE(fa == fb);  // same call fires (or not) in both
        REQUIRE(a.step_count == b.step_count);
        REQUIRE(a.distance_accum_m == b.distance_accum_m);  // bit-identical
    }
}
