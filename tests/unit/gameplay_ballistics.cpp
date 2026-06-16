// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/gameplay_ballistics.cpp — deterministic ranged ballistics.

#include "gameplay/Ballistics.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace psynder;
using namespace psynder::gameplay;

TEST_CASE("ballistics: hitbox multipliers are the canonical values", "[gameplay]") {
    CHECK(hitbox_multiplier(Hitbox::Head) == Catch::Approx(2.0f));
    CHECK(hitbox_multiplier(Hitbox::Body) == Catch::Approx(1.0f));
    CHECK(hitbox_multiplier(Hitbox::Limb) == Catch::Approx(0.7f));

    // Body is the exact baseline.
    CHECK(hitbox_multiplier(Hitbox::Body) == 1.0f);
}

TEST_CASE("ballistics: full damage at or under the falloff start", "[gameplay]") {
    const f32 base = 100.0f;
    const f32 start = 20.0f;
    const f32 end = 60.0f;
    const f32 minf = 0.25f;

    // Point-blank and anywhere up to the start are full damage.
    CHECK(damage_at_distance(base, 0.0f, start, end, minf) == Catch::Approx(base));
    CHECK(damage_at_distance(base, 10.0f, start, end, minf) == Catch::Approx(base));
    CHECK(damage_at_distance(base, start, start, end, minf) == Catch::Approx(base));
}

TEST_CASE("ballistics: minimum damage at and beyond the falloff end", "[gameplay]") {
    const f32 base = 100.0f;
    const f32 start = 20.0f;
    const f32 end = 60.0f;
    const f32 minf = 0.25f;
    const f32 min_damage = base * minf;  // 25

    CHECK(damage_at_distance(base, end, start, end, minf) == Catch::Approx(min_damage));
    // Beyond the end is clamped, not extrapolated below the floor.
    CHECK(damage_at_distance(base, 120.0f, start, end, minf) == Catch::Approx(min_damage));
    CHECK(damage_at_distance(base, 1000.0f, start, end, minf) == Catch::Approx(min_damage));
}

TEST_CASE("ballistics: exact lerp at the falloff midpoint", "[gameplay]") {
    const f32 base = 100.0f;
    const f32 start = 20.0f;
    const f32 end = 60.0f;
    const f32 minf = 0.25f;

    // Midpoint distance = (20 + 60) / 2 = 40 m.
    // min_damage = 25. Halfway between 100 and 25 = 62.5.
    const f32 mid = (start + end) * 0.5f;
    CHECK(damage_at_distance(base, mid, start, end, minf) == Catch::Approx(62.5f));

    // A quarter of the way along (25 m): t = 0.125 -> 100 + (25-100)*0.125 = 90.625.
    CHECK(damage_at_distance(base, 25.0f, start, end, minf) == Catch::Approx(90.625f));
}

TEST_CASE("ballistics: falloff is monotonic non-increasing over a sweep", "[gameplay]") {
    const f32 base = 80.0f;
    const f32 start = 5.0f;
    const f32 end = 50.0f;
    const f32 minf = 0.3f;

    f32 prev = damage_at_distance(base, 0.0f, start, end, minf);
    for (f32 d = 0.0f; d <= 100.0f; d += 0.5f) {
        const f32 cur = damage_at_distance(base, d, start, end, minf);
        CHECK(cur <= prev + 1e-4f);  // never increases with distance
        prev = cur;
    }

    // Floor is honoured and bounds are right.
    CHECK(prev == Catch::Approx(base * minf));
}

TEST_CASE("ballistics: ranged_damage folds in the hitbox multiplier", "[gameplay]") {
    const f32 base = 50.0f;
    const f32 start = 10.0f;
    const f32 end = 40.0f;
    const f32 minf = 0.2f;

    // Headshot at point-blank == base * 2 (full damage, x2).
    CHECK(ranged_damage(base, 0.0f, start, end, minf, Hitbox::Head) ==
          Catch::Approx(base * 2.0f));

    // Body shot at the falloff end == base * min_fraction (x1).
    CHECK(ranged_damage(base, end, start, end, minf, Hitbox::Body) ==
          Catch::Approx(base * minf));

    // Limb shot at point-blank == base * 0.7.
    CHECK(ranged_damage(base, 0.0f, start, end, minf, Hitbox::Limb) ==
          Catch::Approx(base * 0.7f));
}

TEST_CASE("ballistics: degenerate negative distance is treated as point-blank",
          "[gameplay]") {
    const f32 base = 100.0f;
    const f32 start = 10.0f;
    const f32 end = 40.0f;
    const f32 minf = 0.25f;

    CHECK(damage_at_distance(base, -5.0f, start, end, minf) == Catch::Approx(base));
    CHECK(damage_at_distance(base, -1000.0f, start, end, minf) == Catch::Approx(base));
}

TEST_CASE("ballistics: degenerate end <= start collapses to a clean step",
          "[gameplay]") {
    const f32 base = 100.0f;
    const f32 minf = 0.25f;
    const f32 min_damage = base * minf;

    // end == start: step at start.
    CHECK(damage_at_distance(base, 9.99f, 10.0f, 10.0f, minf) == Catch::Approx(base));
    CHECK(damage_at_distance(base, 10.0f, 10.0f, 10.0f, minf) == Catch::Approx(base));  // <= start
    CHECK(damage_at_distance(base, 10.01f, 10.0f, 10.0f, minf) == Catch::Approx(min_damage));

    // end < start: still a clean step at start, no NaN / inf from a bad denom.
    CHECK(damage_at_distance(base, 5.0f, 10.0f, 2.0f, minf) == Catch::Approx(base));
    CHECK(damage_at_distance(base, 10.0f, 10.0f, 2.0f, minf) == Catch::Approx(base));
    CHECK(damage_at_distance(base, 50.0f, 10.0f, 2.0f, minf) == Catch::Approx(min_damage));
}

TEST_CASE("ballistics: min_fraction is clamped into [0,1]", "[gameplay]") {
    const f32 base = 100.0f;
    const f32 start = 10.0f;
    const f32 end = 40.0f;

    // Over-unity clamps to 1 -> no falloff floor below base.
    CHECK(damage_at_distance(base, 100.0f, start, end, 5.0f) == Catch::Approx(base));
    // Negative clamps to 0 -> floor is zero damage at long range.
    CHECK(damage_at_distance(base, 100.0f, start, end, -2.0f) == Catch::Approx(0.0f));
}

TEST_CASE("ballistics: identical inputs give bit-identical f32 (determinism)",
          "[gameplay]") {
    const f32 base = 73.0f;
    const f32 start = 12.5f;
    const f32 end = 47.5f;
    const f32 minf = 0.33f;

    const f32 a = damage_at_distance(base, 31.25f, start, end, minf);
    for (int i = 0; i < 64; ++i) {
        CHECK(damage_at_distance(base, 31.25f, start, end, minf) == a);  // exact ==
    }

    const f32 r = ranged_damage(base, 31.25f, start, end, minf, Hitbox::Head);
    for (int i = 0; i < 64; ++i) {
        CHECK(ranged_damage(base, 31.25f, start, end, minf, Hitbox::Head) == r);
    }
}
