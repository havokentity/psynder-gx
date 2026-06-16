// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/gameplay_ranged_damage.cpp — composable ranged-damage profiles and
// the vertical hit-region classifier.

#include "gameplay/RangedDamage.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace psynder;
using namespace psynder::gameplay;

TEST_CASE("ranged_damage: kNoFalloff is the identity profile at every range",
          "[gameplay]") {
    const f32 base = 100.0f;

    // Point-blank and far out both return the full flat base damage.
    CHECK(resolve_ranged_damage(base, 0.0f, Hitbox::Body, kNoFalloff) ==
          Catch::Approx(base));
    CHECK(resolve_ranged_damage(base, 250.0f, Hitbox::Body, kNoFalloff) ==
          Catch::Approx(base));
    CHECK(resolve_ranged_damage(base, 10000.0f, Hitbox::Body, kNoFalloff) ==
          Catch::Approx(base));

    // A default-constructed profile is also the identity profile.
    const FalloffProfile def{};
    CHECK(resolve_ranged_damage(base, 500.0f, Hitbox::Body, def) ==
          Catch::Approx(base));
}

TEST_CASE("ranged_damage: a profile gives full / floor / ramp across distance",
          "[gameplay]") {
    const f32 base = 100.0f;
    const FalloffProfile p{20.0f, 60.0f, 0.25f};  // start, end, floor
    const f32 floor = base * p.min_fraction;       // 25

    // Inside falloff_start -> full damage.
    CHECK(resolve_ranged_damage(base, 0.0f, Hitbox::Body, p) ==
          Catch::Approx(base));
    CHECK(resolve_ranged_damage(base, p.falloff_start_m, Hitbox::Body, p) ==
          Catch::Approx(base));

    // Beyond falloff_end -> the min_fraction floor.
    CHECK(resolve_ranged_damage(base, p.falloff_end_m, Hitbox::Body, p) ==
          Catch::Approx(floor));
    CHECK(resolve_ranged_damage(base, 500.0f, Hitbox::Body, p) ==
          Catch::Approx(floor));

    // Midpoint of the ramp (40 m) -> strictly between floor and base, and exactly
    // halfway between them: (100 + 25) / 2 = 62.5.
    const f32 mid_d = (p.falloff_start_m + p.falloff_end_m) * 0.5f;
    const f32 mid = resolve_ranged_damage(base, mid_d, Hitbox::Body, p);
    CHECK(mid > floor);
    CHECK(mid < base);
    CHECK(mid == Catch::Approx(62.5f));
}

TEST_CASE("ranged_damage: resolve equals the underlying ranged_damage call",
          "[gameplay]") {
    const f32 base = 80.0f;
    const FalloffProfile p = kRifleFalloff;

    // Cross-check resolve against the raw Ballistics call with the profile's
    // fields unpacked, at a couple of distinct distances and hitboxes.
    CHECK(resolve_ranged_damage(base, 50.0f, Hitbox::Body, p) ==
          Catch::Approx(ranged_damage(base, 50.0f, p.falloff_start_m,
                                      p.falloff_end_m, p.min_fraction,
                                      Hitbox::Body)));
    CHECK(resolve_ranged_damage(base, 120.0f, Hitbox::Limb, p) ==
          Catch::Approx(ranged_damage(base, 120.0f, p.falloff_start_m,
                                      p.falloff_end_m, p.min_fraction,
                                      Hitbox::Limb)));

    // A headshot is exactly double a body shot at the same distance.
    const f32 d = 60.0f;
    const f32 body = resolve_ranged_damage(base, d, Hitbox::Body, p);
    const f32 head = resolve_ranged_damage(base, d, Hitbox::Head, p);
    CHECK(head == Catch::Approx(body * 2.0f));
}

TEST_CASE("ranged_damage: canonical profiles have sane ordered breakpoints",
          "[gameplay]") {
    // Shotgun bottoms out hardest and soonest; rifle reaches furthest.
    CHECK(kShotgunFalloff.falloff_end_m < kPistolFalloff.falloff_end_m);
    CHECK(kPistolFalloff.falloff_end_m < kRifleFalloff.falloff_end_m);
    CHECK(kShotgunFalloff.min_fraction < kPistolFalloff.min_fraction);
    CHECK(kPistolFalloff.min_fraction < kRifleFalloff.min_fraction);

    // Every floor fraction sits inside [0, 1].
    CHECK(kRifleFalloff.min_fraction <= 1.0f);
    CHECK(kShotgunFalloff.min_fraction >= 0.0f);
}

TEST_CASE("ranged_damage: classify_hitbox bins top / middle / bottom bands",
          "[gameplay]") {
    const f32 foot = 0.0f;
    const f32 height = 2.0f;  // a 2 m tall target

    // Top band (frac >= 0.85): 1.95 m / 2 m = 0.975 -> Head.
    CHECK(classify_hitbox(1.95f, foot, height) == Hitbox::Head);
    // Exactly on the head threshold (0.85 * 2 = 1.70 m) -> Head (>=).
    CHECK(classify_hitbox(1.70f, foot, height) == Hitbox::Head);

    // Middle band -> Body (1.0 m -> frac 0.5).
    CHECK(classify_hitbox(1.0f, foot, height) == Hitbox::Body);

    // Bottom band (frac <= 0.30): 0.4 m / 2 m = 0.2 -> Limb.
    CHECK(classify_hitbox(0.4f, foot, height) == Hitbox::Limb);
    // Exactly on the limb threshold (0.30 * 2 = 0.60 m) -> Limb (<=).
    CHECK(classify_hitbox(0.60f, foot, height) == Hitbox::Limb);
}

TEST_CASE("ranged_damage: classify_hitbox clamps out-of-range and works off feet",
          "[gameplay]") {
    const f32 height = 1.8f;

    // Impact above the crown clamps to frac 1 -> Head.
    CHECK(classify_hitbox(100.0f, 0.0f, height) == Hitbox::Head);
    // Impact below the feet clamps to frac 0 -> Limb.
    CHECK(classify_hitbox(-50.0f, 0.0f, height) == Hitbox::Limb);

    // A target standing on a ledge (non-zero foot_y) is classified relative to
    // its own feet: foot_y = 10, height 1.8 -> head band starts at 10 + 1.53.
    CHECK(classify_hitbox(11.7f, 10.0f, height) == Hitbox::Head);
    CHECK(classify_hitbox(10.9f, 10.0f, height) == Hitbox::Body);
    CHECK(classify_hitbox(10.2f, 10.0f, height) == Hitbox::Limb);
}

TEST_CASE("ranged_damage: classify_hitbox guards non-positive height", "[gameplay]") {
    CHECK(classify_hitbox(1.0f, 0.0f, 0.0f) == Hitbox::Body);
    CHECK(classify_hitbox(1.0f, 0.0f, -2.0f) == Hitbox::Body);
}

TEST_CASE("ranged_damage: identical inputs give bit-identical f32 (determinism)",
          "[gameplay]") {
    const f32 base = 73.0f;
    const FalloffProfile p{12.5f, 47.5f, 0.33f};

    const f32 a = resolve_ranged_damage(base, 31.25f, Hitbox::Head, p);
    for (int i = 0; i < 64; ++i) {
        // Exact == on the f32 result, not just Approx — lockstep demands bit
        // identity across repeated calls.
        CHECK(resolve_ranged_damage(base, 31.25f, Hitbox::Head, p) == a);
    }

    const Hitbox h = classify_hitbox(1.37f, 0.0f, 1.8f);
    for (int i = 0; i < 64; ++i) {
        CHECK(classify_hitbox(1.37f, 0.0f, 1.8f) == h);
    }
}
