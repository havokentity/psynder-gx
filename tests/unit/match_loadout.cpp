// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/match_loadout.cpp — per-class spawn loadouts (engine/match/Loadout):
// the class table (sane health, the documented weapon classes, speed mults
// ordered Heavy < Assault < Scout), out-of-range -> Assault, applying a loadout
// to a pawn's Weapon + Health, apply_class composition, the missing-component
// no-op, and bit-for-bit determinism across recomputes.

#include "match/Loadout.h"

#include "gameplay/WeaponLoadout.h"
#include "gameplay/GameplayComponents.h"
#include "scene/World.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace psynder;
using namespace psynder::match;
using psynder::gameplay::Health;
using psynder::gameplay::Weapon;
using psynder::gameplay::WeaponClass;
using psynder::gameplay::weapon_spec;
using psynder::scene::World;

namespace {

// Spawn a pawn carrying both a (zeroed) Weapon and a (zeroed) Health, the shape
// apply_loadout writes into.
Entity spawn_pawn(World& w) {
    const Entity e = w.create();
    w.add(e, Weapon{0.0f, 0.0f, 0.0f, 0, 0.0f});
    w.add(e, Health{0.0f, 0.0f});
    return e;
}

}  // namespace

TEST_CASE("match: each ClassKit has a sane loadout", "[match]") {
    const Loadout assault = class_loadout(ClassKit::Assault);
    const Loadout scout   = class_loadout(ClassKit::Scout);
    const Loadout heavy   = class_loadout(ClassKit::Heavy);

    // The documented weapon classes per kit.
    REQUIRE(assault.primary == WeaponClass::MachineGun);
    REQUIRE(assault.secondary == WeaponClass::Plasma);
    REQUIRE(scout.primary == WeaponClass::Railgun);
    REQUIRE(scout.secondary == WeaponClass::Plasma);
    REQUIRE(heavy.primary == WeaponClass::RocketLauncher);
    REQUIRE(heavy.secondary == WeaponClass::Shotgun);

    // Positive starting health, at the documented values.
    REQUIRE(assault.start_health > 0.0f);
    REQUIRE(scout.start_health > 0.0f);
    REQUIRE(heavy.start_health > 0.0f);
    REQUIRE(assault.start_health == Catch::Approx(100.0f));
    REQUIRE(scout.start_health == Catch::Approx(75.0f));
    REQUIRE(heavy.start_health == Catch::Approx(150.0f));

    // Speed mults ordered Heavy < Assault < Scout (heavy lumbers, scout sprints).
    REQUIRE(heavy.speed_mult < assault.speed_mult);
    REQUIRE(assault.speed_mult < scout.speed_mult);
    REQUIRE(assault.speed_mult == Catch::Approx(1.0f));
    REQUIRE(scout.speed_mult == Catch::Approx(1.2f));
    REQUIRE(heavy.speed_mult == Catch::Approx(0.85f));
}

TEST_CASE("match: an out-of-range ClassKit falls back to Assault", "[match]") {
    const Loadout assault = class_loadout(ClassKit::Assault);
    const Loadout oor     = class_loadout(static_cast<ClassKit>(99u));

    REQUIRE(oor.primary == assault.primary);
    REQUIRE(oor.secondary == assault.secondary);
    REQUIRE(oor.start_health == Catch::Approx(assault.start_health));
    REQUIRE(oor.speed_mult == Catch::Approx(assault.speed_mult));
}

TEST_CASE("match: apply_loadout equips the primary and sets full health",
          "[match]") {
    World w;
    const Entity pawn = spawn_pawn(w);
    const Loadout lo  = class_loadout(ClassKit::Scout);

    apply_loadout(w, pawn, lo);

    // The Weapon now mirrors the primary's archetype (damage is the witness).
    Weapon* wp = w.get<Weapon>(pawn);
    REQUIRE(wp != nullptr);
    const auto spec = weapon_spec(lo.primary);
    REQUIRE(wp->damage == Catch::Approx(spec.damage));
    REQUIRE(wp->fire_interval_s == Catch::Approx(spec.fire_interval_s));
    REQUIRE(wp->ammo == spec.max_ammo);     // full magazine
    REQUIRE(wp->cooldown_s == Catch::Approx(0.0f));  // armed

    // Health spawns full: hp == max_hp == start_health.
    Health* hp = w.get<Health>(pawn);
    REQUIRE(hp != nullptr);
    REQUIRE(hp->hp == Catch::Approx(lo.start_health));
    REQUIRE(hp->max_hp == Catch::Approx(lo.start_health));
    REQUIRE(hp->hp == Catch::Approx(75.0f));
}

TEST_CASE("match: apply_class composes class_loadout and apply_loadout",
          "[match]") {
    World w;
    const Entity a = spawn_pawn(w);
    const Entity b = spawn_pawn(w);

    apply_class(w, a, ClassKit::Heavy);
    apply_loadout(w, b, class_loadout(ClassKit::Heavy));

    Weapon* wa = w.get<Weapon>(a);
    Weapon* wb = w.get<Weapon>(b);
    Health* ha = w.get<Health>(a);
    Health* hb = w.get<Health>(b);
    REQUIRE(wa != nullptr);
    REQUIRE(wb != nullptr);
    REQUIRE(ha != nullptr);
    REQUIRE(hb != nullptr);

    // apply_class(kit) is exactly apply_loadout(class_loadout(kit)).
    REQUIRE(wa->damage == Catch::Approx(wb->damage));
    REQUIRE(wa->fire_interval_s == Catch::Approx(wb->fire_interval_s));
    REQUIRE(wa->ammo == wb->ammo);
    REQUIRE(ha->hp == Catch::Approx(hb->hp));
    REQUIRE(ha->max_hp == Catch::Approx(hb->max_hp));

    // And it matches the Heavy archetype + health.
    const auto spec = weapon_spec(WeaponClass::RocketLauncher);
    REQUIRE(wa->damage == Catch::Approx(spec.damage));
    REQUIRE(ha->max_hp == Catch::Approx(150.0f));
}

TEST_CASE("match: a pawn missing Weapon or Health is a safe no-op", "[match]") {
    World w;

    // Pawn with only Health: Health is set, no Weapon to touch.
    const Entity only_health = w.create();
    w.add(only_health, Health{0.0f, 0.0f});
    apply_class(w, only_health, ClassKit::Assault);
    Health* h = w.get<Health>(only_health);
    REQUIRE(h != nullptr);
    REQUIRE(h->hp == Catch::Approx(100.0f));
    REQUIRE(w.get<Weapon>(only_health) == nullptr);

    // Pawn with only Weapon: Weapon is equipped, no Health to touch.
    const Entity only_weapon = w.create();
    w.add(only_weapon, Weapon{0.0f, 0.0f, 0.0f, 0, 0.0f});
    apply_class(w, only_weapon, ClassKit::Assault);
    Weapon* wp = w.get<Weapon>(only_weapon);
    REQUIRE(wp != nullptr);
    const auto spec = weapon_spec(WeaponClass::MachineGun);
    REQUIRE(wp->damage == Catch::Approx(spec.damage));
    REQUIRE(w.get<Health>(only_weapon) == nullptr);

    // Pawn with neither: left entirely untouched (no crash, still alive).
    const Entity bare = w.create();
    apply_class(w, bare, ClassKit::Assault);
    REQUIRE(w.alive(bare));
    REQUIRE(w.get<Weapon>(bare) == nullptr);
    REQUIRE(w.get<Health>(bare) == nullptr);
}

TEST_CASE("match: loadouts are bit-deterministic across recomputes", "[match]") {
    // The pure table yields identical Loadouts every call.
    for (u32 i = 0; i < kClassKitCount; ++i) {
        const auto kit = static_cast<ClassKit>(i);
        const Loadout x = class_loadout(kit);
        const Loadout y = class_loadout(kit);
        REQUIRE(x.primary == y.primary);
        REQUIRE(x.secondary == y.secondary);
        REQUIRE(x.start_health == y.start_health);  // bit-exact, same input
        REQUIRE(x.speed_mult == y.speed_mult);
    }

    // Applying the same kit to two identical pawns yields identical state.
    World w;
    const Entity a = spawn_pawn(w);
    const Entity b = spawn_pawn(w);
    apply_class(w, a, ClassKit::Scout);
    apply_class(w, b, ClassKit::Scout);

    Weapon* wa = w.get<Weapon>(a);
    Weapon* wb = w.get<Weapon>(b);
    Health* ha = w.get<Health>(a);
    Health* hb = w.get<Health>(b);
    REQUIRE(wa->damage == wb->damage);              // bit-exact
    REQUIRE(wa->fire_interval_s == wb->fire_interval_s);
    REQUIRE(wa->cooldown_s == wb->cooldown_s);
    REQUIRE(wa->ammo == wb->ammo);
    REQUIRE(wa->hit_radius == wb->hit_radius);
    REQUIRE(ha->hp == hb->hp);
    REQUIRE(ha->max_hp == hb->max_hp);
}
