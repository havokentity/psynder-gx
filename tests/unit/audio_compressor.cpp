// SPDX-License-Identifier: MIT
// Psynder — lane 14 unit test: master-bus peak compressor / limiter
// (Compressor.h), the safety net that keeps the summed master bus from clipping.
//
// Verifies the load-bearing properties of engine/audio/Compressor.h:
//   (a) a level at/below threshold gets gain 1.0 (passed untouched);
//   (b) a loud level gets gain < 1 that pulls the output toward
//       threshold + a ratio-compressed slice of the excess;
//   (c) ratio 1 means NO compression even above threshold (gain stays 1.0);
//   (d) the ceiling hard-limits a very loud signal so gain * input <= ceiling;
//   (e) compressor_update attacks DOWN quickly onto a loud target then releases
//       UP slowly back toward unity when the signal drops, never overshooting;
//   (f) a non-positive input_level / zero dt are guarded; and
//   (g) determinism: identical calls are bit-identical.

#include "audio/Compressor.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace psynder;
using namespace psynder::audio;

// A representative master-bus configuration: start compressing at 0.7, soft 4:1
// ratio, a hard 0.95 ceiling, fast attack and a much slower release.
static CompressorParams make_params() noexcept {
    return CompressorParams{
        0.7f,   // threshold
        4.0f,   // ratio (4:1)
        0.95f,  // ceiling
        20.0f,  // attack_per_s  (fast clamp-down)
        2.0f,   // release_per_s (slow recovery)
    };
}

TEST_CASE("audio: a level below threshold passes untouched at unity gain", "[audio][compressor]") {
    const CompressorParams p = make_params();

    // Comfortably under the threshold => no compression.
    REQUIRE(compressor_target_gain(0.3f, p) == Catch::Approx(1.0f).margin(1e-6f));

    // Exactly at the threshold is still the un-compressed knee.
    REQUIRE(compressor_target_gain(p.threshold, p) == Catch::Approx(1.0f).margin(1e-6f));
}

TEST_CASE("audio: a loud level compresses toward threshold plus a ratio slice", "[audio][compressor]") {
    const CompressorParams p = make_params();  // threshold 0.7, ratio 4:1

    const f32 input = 1.5f;  // well above threshold, but the ceiling is high (0.95)
    const f32 g = compressor_target_gain(input, p);
    INFO("gain=" << g << " output=" << (g * input));

    // It attenuates.
    REQUIRE(g < 1.0f);
    REQUIRE(g > 0.0f);

    // The output lands on threshold + (excess / ratio), capped by the ceiling.
    const f32 expected_allowed = p.threshold + (input - p.threshold) / p.ratio;  // 0.9
    const f32 expected_output = expected_allowed < p.ceiling ? expected_allowed : p.ceiling;
    REQUIRE((g * input) == Catch::Approx(expected_output).margin(1e-5f));

    // The compressed output sits above the threshold but below the raw input:
    // it tames the peak rather than slamming it to the threshold.
    REQUIRE((g * input) > p.threshold);
    REQUIRE((g * input) < input);
}

TEST_CASE("audio: ratio of one applies no compression even above threshold", "[audio][compressor]") {
    CompressorParams p = make_params();
    p.ratio = 1.0f;          // 1:1 — no compression
    p.ceiling = 1.0f;        // lift the limiter out of the way

    // Above the threshold but ratio 1 => the whole excess passes => gain 1.0.
    REQUIRE(compressor_target_gain(0.9f, p) == Catch::Approx(1.0f).margin(1e-6f));

    // A ratio below 1 is treated as 1 (never expands / boosts).
    p.ratio = 0.5f;
    const f32 g = compressor_target_gain(0.9f, p);
    REQUIRE(g == Catch::Approx(1.0f).margin(1e-6f));
    REQUIRE(g <= 1.0f);
}

TEST_CASE("audio: the ceiling hard-limits a very loud signal", "[audio][compressor]") {
    const CompressorParams p = make_params();  // ceiling 0.95

    // A range of very loud inputs: every output amplitude stays at/under the
    // ceiling, and the loudest ones are clamped exactly to it (the limiter).
    for (f32 input = 1.0f; input <= 8.0f; input += 0.5f) {
        const f32 g = compressor_target_gain(input, p);
        const f32 out = g * input;
        INFO("input=" << input << " gain=" << g << " output=" << out);
        REQUIRE(out <= p.ceiling + 1e-5f);
        REQUIRE(g > 0.0f);
        REQUIRE(g <= 1.0f);
    }

    // A signal loud enough that even the 4:1 slice exceeds the ceiling is pinned
    // to exactly the ceiling: threshold + (5 - 0.7)/4 = 1.775 > 0.95 => clamp.
    const f32 loud = 5.0f;
    const f32 g_loud = compressor_target_gain(loud, p);
    REQUIRE((g_loud * loud) == Catch::Approx(p.ceiling).margin(1e-5f));
}

TEST_CASE("audio: the envelope attacks down fast then releases up slowly", "[audio][compressor]") {
    const CompressorParams p = make_params();

    CompressorState s{};
    compressor_init(s);
    REQUIRE(compressor_gain(s) == Catch::Approx(1.0f).margin(1e-6f));

    // A sudden loud peak: its instantaneous target is well below unity.
    const f32 loud = 2.0f;
    const f32 target_loud = compressor_target_gain(loud, p);
    REQUIRE(target_loud < 1.0f);

    // ATTACK: a few small blocks pull the gain DOWN toward the loud target. With
    // attack_per_s = 20 it clamps onto it within a handful of milliseconds.
    f32 prev = compressor_gain(s);
    for (int i = 0; i < 8; ++i) {
        compressor_update(s, p, loud, 0.01f);  // 10 ms blocks
        const f32 g = compressor_gain(s);
        REQUIRE(g <= prev + 1e-6f);          // moving down (or already settled)
        REQUIRE(g >= target_loud - 1e-6f);   // never overshoots past the target
        prev = g;
    }
    REQUIRE(compressor_gain(s) == Catch::Approx(target_loud).margin(1e-5f));

    // RELEASE: the loud signal drops to silence; the gain eases back UP toward
    // unity. With release_per_s = 2 it recovers slowly, so a single short block
    // is still well short of unity (it must not snap back instantly).
    const f32 after_attack = compressor_gain(s);
    compressor_update(s, p, 0.0f, 0.01f);  // one 10 ms block of quiet
    const f32 g_release = compressor_gain(s);
    REQUIRE(g_release > after_attack);  // recovering upward
    REQUIRE(g_release < 1.0f);          // but slowly — not yet back to unity

    // Given enough quiet blocks it fully recovers to unity and never exceeds it.
    for (int i = 0; i < 200; ++i) {
        compressor_update(s, p, 0.0f, 0.01f);
        REQUIRE(compressor_gain(s) <= 1.0f);
    }
    REQUIRE(compressor_gain(s) == Catch::Approx(1.0f).margin(1e-5f));
}

TEST_CASE("audio: the compressor guards bad input level and dt", "[audio][compressor]") {
    const CompressorParams p = make_params();

    // A non-positive level has nothing to compress and no divisor: gain 1.0.
    REQUIRE(compressor_target_gain(0.0f, p) == Catch::Approx(1.0f).margin(1e-6f));
    REQUIRE(compressor_target_gain(-1.0f, p) == Catch::Approx(1.0f).margin(1e-6f));

    // A zero / negative dt leaves the smoothed envelope completely unchanged.
    CompressorState s{};
    compressor_init(s);
    compressor_update(s, p, 2.0f, 0.01f);  // move it off unity first
    const f32 before = compressor_gain(s);
    REQUIRE(before < 1.0f);

    compressor_update(s, p, 2.0f, 0.0f);
    REQUIRE(compressor_gain(s) == before);
    compressor_update(s, p, 2.0f, -0.5f);
    REQUIRE(compressor_gain(s) == before);
}

TEST_CASE("audio: compressor results are deterministic across identical calls", "[audio][compressor]") {
    const CompressorParams p = make_params();

    // Same inputs => bit-identical instantaneous gain (same-platform determinism).
    REQUIRE(compressor_target_gain(1.3f, p) == compressor_target_gain(1.3f, p));
    REQUIRE(compressor_target_gain(0.5f, p) == compressor_target_gain(0.5f, p));

    // Two envelopes stepped through the same block sequence stay bit-identical.
    CompressorState a{};
    CompressorState b{};
    compressor_init(a);
    compressor_init(b);
    const f32 levels[] = {1.8f, 1.8f, 1.8f, 0.2f, 0.2f, 1.1f, 0.0f, 0.9f};
    for (const f32 lvl : levels) {
        compressor_update(a, p, lvl, 0.016f);
        compressor_update(b, p, lvl, 0.016f);
        REQUIRE(compressor_gain(a) == compressor_gain(b));
    }
}
