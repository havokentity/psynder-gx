// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/gameplay_combat_resolve.cpp — the shot-resolution integration:
// the ammo gate (Magazine), suppression spread, and QuadDamage scaling composed
// into one begin_shot()/resolve_damage() pair, with the RangedDamage falloff.

#include "gameplay/CombatResolve.h"

#include "gameplay/Ballistics.h"
#include "gameplay/Powerup.h"
#include "gameplay/RangedDamage.h"
#include "gameplay/Reload.h"
#include "gameplay/Suppression.h"

#include "scene/World.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace psynder;
using namespace psynder::gameplay;

namespace {
// A magazine with `n` rounds chambered, not reloading.
Magazine full_mag(i32 n) { return Magazine{n, 30, 10, 2.0f, 0.0f}; }
}  // namespace

TEST_CASE("combat: a shooter with no modifier components fires neutrally",
          "[gameplay]") {
    scene::World w;
    const Entity e = w.create();  // bare shooter, no Magazine/Suppression/Powerups
    const ShotResult r = begin_shot(w, e, /*base_spread_tan=*/0.05f);
    CHECK(r.fired);
    CHECK(r.spread_tan == Catch::Approx(0.05f));  // base, unmodified
    CHECK(r.damage_scale == Catch::Approx(1.0f));
}

TEST_CASE("combat: the magazine gates fire and spends a round", "[gameplay]") {
    scene::World w;
    const Entity e = w.create();
    w.add(e, full_mag(3));

    // A full magazine fires and consumes one round.
    const ShotResult r = begin_shot(w, e, 0.0f);
    CHECK(r.fired);
    CHECK(w.get<Magazine>(e)->in_mag == 2);

    // Drain it: once empty, begin_shot is blocked and spends nothing.
    w.get<Magazine>(e)->in_mag = 0;
    const ShotResult empty = begin_shot(w, e, 0.0f);
    CHECK_FALSE(empty.fired);
    CHECK(w.get<Magazine>(e)->in_mag == 0);
}

TEST_CASE("combat: a reloading shooter cannot fire", "[gameplay]") {
    scene::World w;
    const Entity e = w.create();
    Magazine m = full_mag(5);
    m.reload_left_s = 1.0f;  // mid-reload
    w.add(e, m);
    const ShotResult r = begin_shot(w, e, 0.0f);
    CHECK_FALSE(r.fired);
    CHECK(w.get<Magazine>(e)->in_mag == 5);  // no round spent
}

TEST_CASE("combat: suppression widens the spread cone", "[gameplay]") {
    scene::World w;
    const Entity e = w.create();
    // Fully suppressed (level 1) with a 3x max spread multiplier.
    w.add(e, Suppression{1.0f, 0.5f, 3.0f});
    const ShotResult r = begin_shot(w, e, /*base_spread_tan=*/0.04f);
    CHECK(r.fired);
    CHECK(r.spread_tan == Catch::Approx(0.04f * 3.0f));  // base * suppression mult
    // An unsuppressed shooter keeps the base cone.
    const Entity calm = w.create();
    w.add(calm, Suppression{0.0f, 0.5f, 3.0f});
    CHECK(begin_shot(w, calm, 0.04f).spread_tan == Catch::Approx(0.04f));
}

TEST_CASE("combat: quad damage scales the outgoing damage four times",
          "[gameplay]") {
    scene::World w;
    const Entity e = w.create();
    Powerups p{};
    grant_powerup(p, PowerupKind::QuadDamage, 30.0f);
    w.add(e, p);

    const ShotResult r = begin_shot(w, e, 0.0f);
    CHECK(r.fired);
    CHECK(r.damage_scale == Catch::Approx(4.0f));

    // resolve_damage applies the falloff THEN the quad scale.
    const f32 base = 50.0f;
    const f32 plain = resolve_ranged_damage(base, 10.0f, Hitbox::Body, kNoFalloff);
    const f32 quad = resolve_damage(base, 10.0f, Hitbox::Body, kNoFalloff, r);
    CHECK(quad == Catch::Approx(plain * 4.0f));
}

TEST_CASE("combat: all modifiers compose on one shooter", "[gameplay]") {
    scene::World w;
    const Entity e = w.create();
    w.add(e, full_mag(2));
    w.add(e, Suppression{1.0f, 0.5f, 2.0f});
    Powerups p{};
    grant_powerup(p, PowerupKind::QuadDamage, 5.0f);
    w.add(e, p);

    const ShotResult r = begin_shot(w, e, 0.05f);
    CHECK(r.fired);
    CHECK(w.get<Magazine>(e)->in_mag == 1);          // round spent
    CHECK(r.spread_tan == Catch::Approx(0.10f));      // 0.05 * 2 suppression
    CHECK(r.damage_scale == Catch::Approx(4.0f));     // quad

    // A headshot at range, quad-scaled: falloff(headshot) * 4.
    const f32 dmg = resolve_damage(40.0f, 5.0f, Hitbox::Head, kRifleFalloff, r);
    const f32 ref = resolve_ranged_damage(40.0f, 5.0f, Hitbox::Head, kRifleFalloff) * 4.0f;
    CHECK(dmg == Catch::Approx(ref));
    CHECK(dmg > 0.0f);
}

TEST_CASE("combat: shot resolution is deterministic", "[gameplay][determinism]") {
    auto run = [] {
        scene::World w;
        const Entity e = w.create();
        w.add(e, full_mag(10));
        w.add(e, Suppression{0.5f, 0.5f, 3.0f});
        Powerups p{};
        grant_powerup(p, PowerupKind::QuadDamage, 5.0f);
        w.add(e, p);
        const ShotResult r = begin_shot(w, e, 0.03f);
        return resolve_damage(25.0f, 12.0f, Hitbox::Body, kRifleFalloff, r);
    };
    CHECK(run() == run());
}
