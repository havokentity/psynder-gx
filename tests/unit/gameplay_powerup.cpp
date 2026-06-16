// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/gameplay_powerup.cpp — timed Quake3-style powerups: grant/refresh,
// expiry tick, damage multipliers (Quad out, BattleSuit in), and Regeneration
// overheal. All deterministic (pure algebra, no RNG).

#include "gameplay/GameplayComponents.h"
#include "gameplay/Powerup.h"

#include "scene/World.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace psynder;
using namespace psynder::gameplay;
using psynder::scene::World;

namespace {
Entity spawn_with_powerups(World& w) {
    const Entity e = w.create();
    w.add(e, Powerups{});  // all timers 0 (inactive)
    return e;
}
}  // namespace

TEST_CASE("gameplay: grant sets a timer and refresh never shortens it",
          "[gameplay][powerup]") {
    Powerups p{};
    REQUIRE_FALSE(has_powerup(p, PowerupKind::QuadDamage));

    grant_powerup(p, PowerupKind::QuadDamage, 30.0f);
    REQUIRE(p.time_left[static_cast<u32>(PowerupKind::QuadDamage)] ==
            Catch::Approx(30.0f));

    // A shorter re-grant must NOT shorten the active powerup.
    grant_powerup(p, PowerupKind::QuadDamage, 5.0f);
    REQUIRE(p.time_left[static_cast<u32>(PowerupKind::QuadDamage)] ==
            Catch::Approx(30.0f));

    // A longer re-grant refreshes upward.
    grant_powerup(p, PowerupKind::QuadDamage, 45.0f);
    REQUIRE(p.time_left[static_cast<u32>(PowerupKind::QuadDamage)] ==
            Catch::Approx(45.0f));

    // A negative duration is clamped to 0 and does not shorten.
    grant_powerup(p, PowerupKind::Haste, -3.0f);
    REQUIRE_FALSE(has_powerup(p, PowerupKind::Haste));
}

TEST_CASE("gameplay: has_powerup reflects per-kind state", "[gameplay][powerup]") {
    Powerups p{};
    grant_powerup(p, PowerupKind::Haste, 10.0f);
    REQUIRE(has_powerup(p, PowerupKind::Haste));
    REQUIRE_FALSE(has_powerup(p, PowerupKind::QuadDamage));
    REQUIRE_FALSE(has_powerup(p, PowerupKind::Regeneration));
    REQUIRE_FALSE(has_powerup(p, PowerupKind::BattleSuit));
}

TEST_CASE("gameplay: tick decrements timers and clamps at zero on expiry",
          "[gameplay][powerup]") {
    World w;
    const Entity e = spawn_with_powerups(w);
    grant_powerup(*w.get<Powerups>(e), PowerupKind::QuadDamage, 1.0f);
    grant_powerup(*w.get<Powerups>(e), PowerupKind::BattleSuit, 2.0f);

    tick_powerups(w, 0.5f);
    REQUIRE(w.get<Powerups>(e)->time_left[static_cast<u32>(
                PowerupKind::QuadDamage)] == Catch::Approx(0.5f));
    REQUIRE(w.get<Powerups>(e)->time_left[static_cast<u32>(
                PowerupKind::BattleSuit)] == Catch::Approx(1.5f));

    // Drive QuadDamage past expiry: it clamps to exactly 0 (never negative).
    tick_powerups(w, 0.75f);
    REQUIRE(w.get<Powerups>(e)->time_left[static_cast<u32>(
                PowerupKind::QuadDamage)] == Catch::Approx(0.0f));
    REQUIRE_FALSE(has_powerup(*w.get<Powerups>(e), PowerupKind::QuadDamage));
    // BattleSuit still ticking.
    REQUIRE(has_powerup(*w.get<Powerups>(e), PowerupKind::BattleSuit));
    REQUIRE(w.get<Powerups>(e)->time_left[static_cast<u32>(
                PowerupKind::BattleSuit)] == Catch::Approx(0.75f));
}

TEST_CASE("gameplay: damage_multiplier is 4x under QuadDamage else 1x",
          "[gameplay][powerup]") {
    Powerups p{};
    REQUIRE(damage_multiplier(p) == Catch::Approx(1.0f));
    grant_powerup(p, PowerupKind::QuadDamage, 5.0f);
    REQUIRE(damage_multiplier(p) == Catch::Approx(4.0f));
    // Other powerups do not affect the outgoing multiplier.
    Powerups q{};
    grant_powerup(q, PowerupKind::Haste, 5.0f);
    REQUIRE(damage_multiplier(q) == Catch::Approx(1.0f));
}

TEST_CASE("gameplay: incoming_damage_multiplier is half under BattleSuit else 1x",
          "[gameplay][powerup]") {
    Powerups p{};
    REQUIRE(incoming_damage_multiplier(p) == Catch::Approx(1.0f));
    grant_powerup(p, PowerupKind::BattleSuit, 5.0f);
    REQUIRE(incoming_damage_multiplier(p) == Catch::Approx(0.5f));
    // QuadDamage does not affect incoming damage.
    Powerups q{};
    grant_powerup(q, PowerupKind::QuadDamage, 5.0f);
    REQUIRE(incoming_damage_multiplier(q) == Catch::Approx(1.0f));
}

TEST_CASE("gameplay: regeneration heals over time up to the overheal cap",
          "[gameplay][powerup]") {
    World w;
    const Entity e = w.create();
    w.add(e, Health{100.0f, 100.0f});
    w.add(e, Powerups{});
    grant_powerup(*w.get<Powerups>(e), PowerupKind::Regeneration, 100.0f);

    constexpr f32 dt = 1.0f / 100.0f;  // 10 ms
    constexpr f32 hp_per_s = 15.0f;    // Quake3 regen rate
    constexpr f32 cap = 200.0f;        // overheal ceiling

    // One step heals hp_per_s * dt past the nominal max (overheal).
    tick_regeneration(w, dt, hp_per_s, cap);
    REQUIRE(w.get<Health>(e)->hp == Catch::Approx(100.0f + hp_per_s * dt));

    // Drive it to the cap: 200 hp at 15 hp/s from 100 takes < 7 s -> 700 steps.
    for (int i = 0; i < 700; ++i) tick_regeneration(w, dt, hp_per_s, cap);
    REQUIRE(w.get<Health>(e)->hp == Catch::Approx(cap));

    // Further ticks do not exceed the cap (clamped, deterministic).
    tick_regeneration(w, dt, hp_per_s, cap);
    REQUIRE(w.get<Health>(e)->hp == Catch::Approx(cap));
}

TEST_CASE("gameplay: regeneration only heals entities with the powerup active",
          "[gameplay][powerup]") {
    World w;
    const Entity healed = w.create();
    w.add(healed, Health{50.0f, 100.0f});
    w.add(healed, Powerups{});
    grant_powerup(*w.get<Powerups>(healed), PowerupKind::Regeneration, 100.0f);

    const Entity ignored = w.create();
    w.add(ignored, Health{50.0f, 100.0f});
    w.add(ignored, Powerups{});  // no Regeneration granted

    tick_regeneration(w, 1.0f, 10.0f, 200.0f);
    REQUIRE(w.get<Health>(healed)->hp == Catch::Approx(60.0f));
    REQUIRE(w.get<Health>(ignored)->hp == Catch::Approx(50.0f));  // untouched
}

TEST_CASE("gameplay: powerups are deterministic across worlds",
          "[gameplay][powerup][determinism]") {
    const auto run = []() {
        World w;
        const Entity a = w.create();
        w.add(a, Health{100.0f, 100.0f});
        w.add(a, Powerups{});
        grant_powerup(*w.get<Powerups>(a), PowerupKind::QuadDamage, 3.0f);
        grant_powerup(*w.get<Powerups>(a), PowerupKind::Regeneration, 5.0f);

        const Entity b = w.create();
        w.add(b, Health{80.0f, 100.0f});
        w.add(b, Powerups{});
        grant_powerup(*w.get<Powerups>(b), PowerupKind::BattleSuit, 4.0f);
        grant_powerup(*w.get<Powerups>(b), PowerupKind::Regeneration, 2.0f);

        constexpr f32 dt = 1.0f / 120.0f;
        for (int step = 0; step < 300; ++step) {
            tick_regeneration(w, dt, 15.0f, 200.0f);
            tick_powerups(w, dt);
        }
        return std::vector<f32>{
            w.get<Health>(a)->hp, w.get<Health>(b)->hp,
            w.get<Powerups>(a)->time_left[static_cast<u32>(
                PowerupKind::QuadDamage)],
            w.get<Powerups>(b)->time_left[static_cast<u32>(
                PowerupKind::BattleSuit)]};
    };
    REQUIRE(run() == run());
}
