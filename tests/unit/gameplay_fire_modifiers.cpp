// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/gameplay_fire_modifiers.cpp — proves fire_hitscan now runs through
// CombatResolve: a Magazine gates the shot (empty => no fire), and Powerups
// (Quad) scale the damage it deals. A shooter with none of those components
// fires exactly as before.

#include "gameplay/Weapons.h"

#include "gameplay/GameplayComponents.h"
#include "gameplay/Powerup.h"
#include "gameplay/Reload.h"

#include "scene/GxComponents.h"  // TransformWS
#include "scene/World.h"

#include "math/Math.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace psynder;
using namespace psynder::gameplay;
using psynder::scene::TransformWS;
using psynder::scene::World;

namespace {

Entity spawn_shooter(World& w) {
    const Entity e = w.create();
    TransformWS t{};
    t.mtw = math::translate({0.0f, 0.0f, 0.0f});
    w.add(e, t);
    // damage 20, fast fire, infinite legacy ammo, generous hitbox.
    w.add(e, Weapon{20.0f, 0.1f, 0.0f, -1, 0.7f});
    w.add(e, Score{0, 0});
    return e;
}

Entity spawn_target(World& w, f32 x) {
    const Entity e = w.create();
    TransformWS t{};
    t.mtw = math::translate({x, 0.0f, 0.0f});
    w.add(e, t);
    w.add(e, Health{100.0f, 100.0f});  // armor-free: hp drop == damage
    w.add(e, Score{0, 0});
    return e;
}

}  // namespace

TEST_CASE("fire mods: a magazine gates fire_hitscan when empty", "[gameplay]") {
    World w;
    const Entity shooter = spawn_shooter(w);
    w.add(shooter, Magazine{1, 0, 10, 2.0f, 0.0f});  // exactly one round
    const Entity target = spawn_target(w, 5.0f);

    // First shot fires (consumes the round) and hits.
    const Entity hit = fire_hitscan(w, shooter, {0, 0, 0}, {1, 0, 0});
    CHECK(hit.raw == target.raw);
    CHECK(w.get<Magazine>(shooter)->in_mag == 0);
    CHECK(w.get<Health>(target)->hp == Catch::Approx(80.0f));

    // Reset the weapon cooldown so only the (now-empty) magazine can block it.
    w.get<Weapon>(shooter)->cooldown_s = 0.0f;
    const Entity blocked = fire_hitscan(w, shooter, {0, 0, 0}, {1, 0, 0});
    CHECK_FALSE(blocked.valid());                          // magazine empty => no shot
    CHECK(w.get<Health>(target)->hp == Catch::Approx(80.0f));  // no further damage
}

TEST_CASE("fire mods: quad damage scales fire_hitscan damage four times",
          "[gameplay]") {
    // Plain shooter: 20 damage.
    World plain;
    const Entity ps = spawn_shooter(plain);
    const Entity pt = spawn_target(plain, 5.0f);
    fire_hitscan(plain, ps, {0, 0, 0}, {1, 0, 0});
    CHECK(plain.get<Health>(pt)->hp == Catch::Approx(80.0f));  // 100 - 20

    // Quad shooter: same weapon, 4x damage.
    World quad;
    const Entity qs = spawn_shooter(quad);
    Powerups pw{};
    grant_powerup(pw, PowerupKind::QuadDamage, 30.0f);
    quad.add(qs, pw);
    const Entity qt = spawn_target(quad, 5.0f);
    fire_hitscan(quad, qs, {0, 0, 0}, {1, 0, 0});
    CHECK(quad.get<Health>(qt)->hp == Catch::Approx(20.0f));  // 100 - 80 (20 x 4)
}

TEST_CASE("fire mods: a bare shooter is unaffected by the wiring", "[gameplay]") {
    // No Magazine/Suppression/Powerups => identical to the legacy path.
    World w;
    const Entity s = spawn_shooter(w);
    const Entity tgt = spawn_target(w, 5.0f);
    const Entity hit = fire_hitscan(w, s, {0, 0, 0}, {1, 0, 0});
    CHECK(hit.raw == tgt.raw);
    CHECK(w.get<Health>(tgt)->hp == Catch::Approx(80.0f));  // plain 20 damage
}
