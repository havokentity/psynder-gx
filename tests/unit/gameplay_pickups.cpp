// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/gameplay_pickups.cpp — kill crediting (frags/deaths) + floor
// pickups (health/ammo) with respawn, all deterministic.

#include "gameplay/Damage.h"
#include "gameplay/GameplayComponents.h"
#include "gameplay/Pickups.h"
#include "gameplay/Weapons.h"

#include "scene/GxComponents.h"
#include "scene/World.h"

#include "math/Math.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <vector>

using namespace psynder;
using namespace psynder::gameplay;
using psynder::scene::TransformWS;
using psynder::scene::World;

namespace {
Entity spawn_player(World& w, math::Vec3 pos, f32 hp) {
    const Entity e = w.create();
    w.add(e, Health{hp, hp});
    w.add(e, Score{0, 0});
    TransformWS t{};
    t.mtw = math::translate(pos);
    t.prev_mtw = t.mtw;
    w.add(e, t);
    return e;
}
}  // namespace

TEST_CASE("gameplay: a kill credits a frag to the killer and a death to the victim",
          "[gameplay][score]") {
    World w;
    const Entity killer = spawn_player(w, {0, 0, 0}, 100.0f);
    w.add(killer, Weapon{120.0f, 0.5f, 0.0f, 10, 0.5f});  // one-shot kill
    const Entity victim = spawn_player(w, {5, 0, 0}, 100.0f);

    REQUIRE(fire_hitscan(w, killer, {0, 0, 0}, {1, 0, 0}) == victim);
    REQUIRE(w.get<Health>(victim)->hp == Catch::Approx(0.0f));
    REQUIRE(w.get<Score>(killer)->frags == 1u);
    REQUIRE(w.get<Score>(killer)->deaths == 0u);
    REQUIRE(w.get<Score>(victim)->deaths == 1u);
    REQUIRE(w.get<Score>(victim)->frags == 0u);
}

TEST_CASE("gameplay: a self-kill is a death but not a frag", "[gameplay][score]") {
    World w;
    const Entity e = spawn_player(w, {0, 0, 0}, 50.0f);
    REQUIRE(damage_credited(w, e, e, 80.0f));
    REQUIRE(w.get<Score>(e)->frags == 0u);
    REQUIRE(w.get<Score>(e)->deaths == 1u);
}

TEST_CASE("gameplay: a health pickup heals (capped) then respawns",
          "[gameplay][pickups]") {
    World w;
    const Entity player = spawn_player(w, {0, 0, 0}, 40.0f);
    w.get<Health>(player)->hp = 40.0f;  // wounded; max 40? set max via Health
    // Give a higher max so healing is visible.
    w.get<Health>(player)->max_hp = 100.0f;
    const Entity pick = spawn_pickup(w, PickupKind::Health, 35.0f, {0, 0, 0},
                                     /*radius=*/1.0f, /*respawn=*/2.0f);

    tick_pickups(w, 1.0f / 120.0f);
    REQUIRE(w.get<Health>(player)->hp == Catch::Approx(75.0f));
    REQUIRE(w.get<Pickup>(pick)->cooldown_s > 0.0f);  // consumed -> inactive

    // No further heal while inactive.
    tick_pickups(w, 1.0f / 120.0f);
    REQUIRE(w.get<Health>(player)->hp == Catch::Approx(75.0f));

    // After the respawn delay it grants again (cap at max_hp = 100).
    for (int i = 0; i < 250; ++i) tick_pickups(w, 1.0f / 120.0f);  // > 2 s
    REQUIRE(w.get<Health>(player)->hp == Catch::Approx(100.0f));
}

TEST_CASE("gameplay: an ammo pickup refills the player's weapon",
          "[gameplay][pickups]") {
    World w;
    const Entity player = spawn_player(w, {0, 0, 0}, 100.0f);
    w.add(player, Weapon{20.0f, 0.1f, 0.0f, 5, 0.5f});
    spawn_pickup(w, PickupKind::Ammo, 30.0f, {0, 0, 0}, 1.0f, 5.0f);
    tick_pickups(w, 1.0f / 120.0f);
    REQUIRE(w.get<Weapon>(player)->ammo == 35);
}

TEST_CASE("gameplay: a pickup out of range is not taken", "[gameplay][pickups]") {
    World w;
    spawn_player(w, {0, 0, 0}, 50.0f);
    const Entity pick = spawn_pickup(w, PickupKind::Health, 25.0f, {10, 0, 0},
                                     1.0f, 2.0f);
    tick_pickups(w, 1.0f / 120.0f);
    REQUIRE(w.get<Pickup>(pick)->cooldown_s == Catch::Approx(0.0f));  // still active
}

TEST_CASE("gameplay: scoring + pickups are deterministic across worlds",
          "[gameplay][determinism]") {
    const auto run = []() {
        World w;
        const Entity a = spawn_player(w, {0, 0, 0}, 100.0f);
        w.add(a, Weapon{40.0f, 0.1f, 0.0f, -1, 0.5f});
        const Entity b = spawn_player(w, {5, 0, 0}, 100.0f);
        spawn_pickup(w, PickupKind::Health, 10.0f, {0, 0, 0}, 1.0f, 1.0f);
        std::vector<Entity> scratch;
        constexpr f32 dt = 1.0f / 120.0f;
        for (int step = 0; step < 150; ++step) {
            fire_hitscan(w, a, {0, 0, 0}, {1, 0, 0});
            tick_weapons(w, dt);
            tick_pickups(w, dt);
            update_respawns(w, dt, scratch);
        }
        return std::vector<u32>{w.get<Score>(a)->frags, w.get<Score>(b)->deaths};
    };
    REQUIRE(run() == run());
}
