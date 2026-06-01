// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/gameplay_shield.cpp — the regenerating energy shield: absorb-then-
// overflow, delayed recharge, and determinism.

#include "gameplay/Shield.h"

#include "gameplay/Damage.h"
#include "gameplay/GameplayComponents.h"

#include "scene/World.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace psynder;
using namespace psynder::gameplay;

namespace {

Entity spawn_attacker(scene::World& w) {
    const Entity e = w.create();
    w.add(e, Score{0, 0});
    return e;
}

// Shield 50/50, recharge 10/s after a 2 s delay; Health 100.
Entity spawn_shielded(scene::World& w) {
    const Entity e = w.create();
    w.add(e, Shield{50.0f, 50.0f, 10.0f, 2.0f, 0.0f});
    w.add(e, Health{100.0f, 100.0f});
    w.add(e, Score{0, 0});
    return e;
}

}  // namespace

TEST_CASE("shield: a small hit only drains the shield", "[gameplay]") {
    scene::World w;
    const Entity a = spawn_attacker(w);
    const Entity v = spawn_shielded(w);
    CHECK_FALSE(damage_shielded(w, a, v, 30.0f));
    CHECK(w.get<Shield>(v)->current == Catch::Approx(20.0f));
    CHECK(w.get<Health>(v)->hp == Catch::Approx(100.0f));  // health untouched
}

TEST_CASE("shield: overflow past the shield reaches health", "[gameplay]") {
    scene::World w;
    const Entity a = spawn_attacker(w);
    const Entity v = spawn_shielded(w);
    CHECK_FALSE(damage_shielded(w, a, v, 70.0f));  // 50 shield + 20 overflow
    CHECK(w.get<Shield>(v)->current == Catch::Approx(0.0f));
    CHECK(w.get<Health>(v)->hp == Catch::Approx(80.0f));
}

TEST_CASE("shield: a lethal overflow kills and credits the attacker", "[gameplay]") {
    scene::World w;
    const Entity a = spawn_attacker(w);
    const Entity v = spawn_shielded(w);
    CHECK(damage_shielded(w, a, v, 150.0f));  // 50 shield + 100 overflow == lethal
    CHECK(w.get<Health>(v)->hp <= 0.0f);
    CHECK(w.get<Score>(a)->frags == 1u);
}

TEST_CASE("shield: recharge waits for the delay then refills without overshoot",
          "[gameplay]") {
    scene::World w;
    const Entity a = spawn_attacker(w);
    const Entity v = spawn_shielded(w);
    damage_shielded(w, a, v, 50.0f);  // drain the shield, time_since_hit -> 0
    REQUIRE(w.get<Shield>(v)->current == Catch::Approx(0.0f));

    // Within the 2 s delay (strict >): no recharge yet.
    tick_shields(w, 1.0f);  // t=1
    tick_shields(w, 1.0f);  // t=2 (not > 2)
    CHECK(w.get<Shield>(v)->current == Catch::Approx(0.0f));

    // Past the delay: recharges rate*dt each tick.
    tick_shields(w, 1.0f);  // t=3 > 2 -> +10
    CHECK(w.get<Shield>(v)->current == Catch::Approx(10.0f));

    // Recharge to full and never overshoot the cap.
    for (int i = 0; i < 20; ++i) tick_shields(w, 1.0f);
    CHECK(w.get<Shield>(v)->current == Catch::Approx(50.0f));
}

TEST_CASE("shield: an unshielded victim takes the full hit", "[gameplay]") {
    scene::World w;
    const Entity a = spawn_attacker(w);
    const Entity bare = w.create();
    w.add(bare, Health{100.0f, 100.0f});
    w.add(bare, Score{0, 0});
    CHECK_FALSE(damage_shielded(w, a, bare, 30.0f));
    CHECK(w.get<Health>(bare)->hp == Catch::Approx(70.0f));
}

TEST_CASE("shield: a dead actor does not recharge", "[gameplay]") {
    scene::World w;
    const Entity a = spawn_attacker(w);
    const Entity v = spawn_shielded(w);
    damage_shielded(w, a, v, 200.0f);  // kill it (shield + lethal overflow)
    REQUIRE(w.get<Health>(v)->hp <= 0.0f);
    const f32 shield_after_death = w.get<Shield>(v)->current;
    for (int i = 0; i < 10; ++i) tick_shields(w, 1.0f);
    CHECK(w.get<Shield>(v)->current == Catch::Approx(shield_after_death));  // frozen
}

TEST_CASE("shield: effective hp sums shield and health", "[gameplay]") {
    scene::World w;
    const Entity a = spawn_attacker(w);
    const Entity v = spawn_shielded(w);
    CHECK(effective_hp(w, v) == Catch::Approx(150.0f));  // 50 + 100
    damage_shielded(w, a, v, 20.0f);
    CHECK(effective_hp(w, v) == Catch::Approx(130.0f));
}

TEST_CASE("shield: the systems are deterministic", "[gameplay][determinism]") {
    auto run = [] {
        scene::World w;
        const Entity a = spawn_attacker(w);
        const Entity v = spawn_shielded(w);
        damage_shielded(w, a, v, 35.0f);
        for (int i = 0; i < 6; ++i) tick_shields(w, 0.7f);
        damage_shielded(w, a, v, 12.0f);
        for (int i = 0; i < 4; ++i) tick_shields(w, 0.7f);
        return effective_hp(w, v);
    };
    CHECK(run() == run());
}
