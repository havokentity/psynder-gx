// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/gameplay_status_effect.cpp — timed damage-over-time status effects
// (Burn / Poison / Bleed). Covers: a Burn drains health by ~dps*elapsed then
// expires (no further damage); re-applying refreshes the timer (longer total)
// and takes the higher dps; a lethal DoT kills + credits the source a frag; two
// kinds tick together; has_status reflects active state; a dead entity takes no
// DoT; and the whole thing is bit-reproducible across worlds.

#include "gameplay/GameplayComponents.h"
#include "gameplay/StatusEffect.h"

#include "scene/World.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace psynder;
using namespace psynder::gameplay;
using psynder::scene::World;

namespace {
Entity spawn_victim(World& w, f32 hp) {
    const Entity e = w.create();
    w.add(e, Health{hp, hp});
    w.add(e, StatusEffects{});  // all slots inactive
    return e;
}

f32 hp_of(World& w, Entity e) { return w.get<Health>(e)->hp; }
}  // namespace

TEST_CASE("gameplay: apply_status sets a kind and has_status reflects it",
          "[gameplay][status]") {
    StatusEffects s{};
    REQUIRE_FALSE(has_status(s, StatusKind::Burn));

    apply_status(s, StatusKind::Burn, 4.0f, 10.0f, Entity{42u});
    REQUIRE(has_status(s, StatusKind::Burn));
    REQUIRE_FALSE(has_status(s, StatusKind::Poison));
    REQUIRE_FALSE(has_status(s, StatusKind::Bleed));
    REQUIRE(s.time_left[static_cast<u32>(StatusKind::Burn)] == Catch::Approx(4.0f));
    REQUIRE(s.dps[static_cast<u32>(StatusKind::Burn)] == Catch::Approx(10.0f));
    REQUIRE(s.source[static_cast<u32>(StatusKind::Burn)] == 42u);
}

TEST_CASE("gameplay: a Burn drains health by dps times elapsed then expires",
          "[gameplay][status]") {
    World w;
    const Entity attacker = w.create();
    const Entity victim = spawn_victim(w, 100.0f);
    apply_status(*w.get<StatusEffects>(victim), StatusKind::Burn, 2.0f, 10.0f,
                 attacker);

    constexpr f32 dt = 0.5f;
    // After 1 s (two ticks) at 10 dps -> 10 damage dealt.
    tick_status(w, dt);
    tick_status(w, dt);
    REQUIRE(hp_of(w, victim) == Catch::Approx(90.0f));
    REQUIRE(has_status(*w.get<StatusEffects>(victim), StatusKind::Burn));

    // Cross the 2 s duration (two more ticks) -> 20 total damage, then expires.
    tick_status(w, dt);
    tick_status(w, dt);
    REQUIRE(hp_of(w, victim) == Catch::Approx(80.0f));
    REQUIRE_FALSE(has_status(*w.get<StatusEffects>(victim), StatusKind::Burn));

    // Expired: further ticks deal no more damage.
    tick_status(w, dt);
    tick_status(w, dt);
    REQUIRE(hp_of(w, victim) == Catch::Approx(80.0f));
}

TEST_CASE("gameplay: re-applying refreshes the timer and takes the higher dps",
          "[gameplay][status]") {
    StatusEffects s{};
    apply_status(s, StatusKind::Poison, 3.0f, 5.0f, Entity{1u});

    // A shorter, weaker re-apply must NOT shorten the timer nor lower the rate.
    apply_status(s, StatusKind::Poison, 1.0f, 2.0f, Entity{2u});
    REQUIRE(s.time_left[static_cast<u32>(StatusKind::Poison)] == Catch::Approx(3.0f));
    REQUIRE(s.dps[static_cast<u32>(StatusKind::Poison)] == Catch::Approx(5.0f));

    // A longer, stronger re-apply refreshes the timer up and raises the rate.
    apply_status(s, StatusKind::Poison, 8.0f, 12.0f, Entity{3u});
    REQUIRE(s.time_left[static_cast<u32>(StatusKind::Poison)] == Catch::Approx(8.0f));
    REQUIRE(s.dps[static_cast<u32>(StatusKind::Poison)] == Catch::Approx(12.0f));
    // The most recent application owns the kill credit.
    REQUIRE(s.source[static_cast<u32>(StatusKind::Poison)] == 3u);

    // A negative duration is clamped to 0 and never shortens an active effect.
    apply_status(s, StatusKind::Bleed, -2.0f, 1.0f, Entity{4u});
    REQUIRE_FALSE(has_status(s, StatusKind::Bleed));
}

TEST_CASE("gameplay: re-applying extends the total damage window",
          "[gameplay][status]") {
    World w;
    const Entity attacker = w.create();
    const Entity victim = spawn_victim(w, 100.0f);
    constexpr f32 dt = 0.5f;

    apply_status(*w.get<StatusEffects>(victim), StatusKind::Burn, 1.0f, 10.0f,
                 attacker);
    // One tick (0.5 s) of the original 1 s effect: 5 damage.
    tick_status(w, dt);
    REQUIRE(hp_of(w, victim) == Catch::Approx(95.0f));

    // Refresh to a longer window with a higher rate while still active.
    apply_status(*w.get<StatusEffects>(victim), StatusKind::Burn, 2.0f, 20.0f,
                 attacker);
    // The refreshed effect now runs four more ticks (2 s) at 20 dps: 40 damage.
    for (int i = 0; i < 4; ++i) tick_status(w, dt);
    REQUIRE(hp_of(w, victim) == Catch::Approx(55.0f));
    REQUIRE_FALSE(has_status(*w.get<StatusEffects>(victim), StatusKind::Burn));
}

TEST_CASE("gameplay: a lethal DoT kills the victim and credits the source a frag",
          "[gameplay][status]") {
    World w;
    const Entity attacker = w.create();
    w.add(attacker, Score{0u, 0u});

    const Entity victim = w.create();
    w.add(victim, Health{30.0f, 30.0f});
    w.add(victim, Score{0u, 0u});
    w.add(victim, StatusEffects{});
    apply_status(*w.get<StatusEffects>(victim), StatusKind::Bleed, 5.0f, 20.0f,
                 attacker);

    // 20 dps over 2 s (two 1 s ticks) = 40 damage > 30 hp -> dead.
    tick_status(w, 1.0f);
    REQUIRE(hp_of(w, victim) == Catch::Approx(10.0f));
    REQUIRE(w.get<Dead>(victim) == nullptr);

    tick_status(w, 1.0f);
    REQUIRE(hp_of(w, victim) == Catch::Approx(0.0f));
    REQUIRE(w.get<Dead>(victim) != nullptr);
    REQUIRE(w.get<Score>(attacker)->frags == 1u);
    REQUIRE(w.get<Score>(victim)->deaths == 1u);
}

TEST_CASE("gameplay: two effects (Burn and Poison) both tick on one entity",
          "[gameplay][status]") {
    World w;
    const Entity attacker = w.create();
    const Entity victim = spawn_victim(w, 100.0f);
    apply_status(*w.get<StatusEffects>(victim), StatusKind::Burn, 3.0f, 10.0f,
                 attacker);
    apply_status(*w.get<StatusEffects>(victim), StatusKind::Poison, 3.0f, 4.0f,
                 attacker);

    REQUIRE(has_status(*w.get<StatusEffects>(victim), StatusKind::Burn));
    REQUIRE(has_status(*w.get<StatusEffects>(victim), StatusKind::Poison));

    // One 1 s tick: 10 (burn) + 4 (poison) = 14 damage.
    tick_status(w, 1.0f);
    REQUIRE(hp_of(w, victim) == Catch::Approx(86.0f));
}

TEST_CASE("gameplay: a dead entity takes no DoT and its effects clear",
          "[gameplay][status]") {
    World w;
    const Entity attacker = w.create();
    const Entity victim = w.create();
    w.add(victim, Health{50.0f, 50.0f});
    w.add(victim, StatusEffects{});
    w.add(victim, Dead{3.0f});  // already a corpse
    apply_status(*w.get<StatusEffects>(victim), StatusKind::Burn, 5.0f, 10.0f,
                 attacker);

    tick_status(w, 1.0f);
    REQUIRE(hp_of(w, victim) == Catch::Approx(50.0f));  // untouched
    REQUIRE_FALSE(has_status(*w.get<StatusEffects>(victim), StatusKind::Burn));
}

TEST_CASE("gameplay: status DoT is deterministic across worlds",
          "[gameplay][status][determinism]") {
    const auto run = []() {
        World w;
        std::vector<Entity> es;
        for (int i = 0; i < 8; ++i) {
            const Entity attacker = w.create();
            const Entity victim = w.create();
            w.add(victim, Health{100.0f, 100.0f});
            w.add(victim, StatusEffects{});
            apply_status(*w.get<StatusEffects>(victim), StatusKind::Burn,
                         4.0f, 1.0f + static_cast<f32>(i % 3), attacker);
            apply_status(*w.get<StatusEffects>(victim), StatusKind::Poison,
                         3.0f, 2.0f, attacker);
            es.push_back(victim);
        }
        constexpr f32 dt = 1.0f / 60.0f;
        for (int step = 0; step < 360; ++step) tick_status(w, dt);

        std::vector<f32> out;
        for (Entity e : es) out.push_back(w.get<Health>(e)->hp);
        return out;
    };
    const std::vector<f32> a = run();
    const std::vector<f32> b = run();
    REQUIRE(a == b);
}
