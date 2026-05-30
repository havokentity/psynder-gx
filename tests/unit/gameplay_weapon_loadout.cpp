// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/gameplay_weapon_loadout.cpp — the canonical FPS arsenal as data:
// every archetype is sane, hitscan-vs-projectile is correct, equip fills the
// Weapon component ready-to-fire, the relative balance holds (rail out-damages
// the chaingun per shot; the chaingun out-fires it; the shotgun falls off harder
// than the rail), out-of-range is a safe default, and the table is deterministic.

#include "gameplay/WeaponLoadout.h"
#include "gameplay/GameplayComponents.h"
#include "gameplay/RangedDamage.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <iterator>  // std::size

using namespace psynder;
using namespace psynder::gameplay;

namespace {

// The five canonical classes, in table order — iterated by the sanity sweep.
constexpr WeaponClass kAllClasses[] = {
    WeaponClass::Railgun, WeaponClass::RocketLauncher, WeaponClass::Shotgun,
    WeaponClass::MachineGun, WeaponClass::Plasma,
};

}  // namespace

TEST_CASE("loadout: every weapon spec is sane", "[gameplay]") {
    static_assert(kWeaponClassCount == 5, "arsenal size frozen");
    REQUIRE(std::size(kAllClasses) == kWeaponClassCount);

    for (WeaponClass c : kAllClasses) {
        const WeaponSpec s = weapon_spec(c);
        CHECK(s.damage > 0.0f);           // every weapon deals damage
        CHECK(s.fire_interval_s > 0.0f);  // a positive, finite refire gate
        CHECK(s.max_ammo != 0);           // a real magazine (or < 0 = infinite)
        CHECK(s.hit_radius > 0.0f);       // a real target hitbox sphere
        // min_fraction is a fraction of base in [0, 1].
        CHECK(s.falloff.min_fraction >= 0.0f);
        CHECK(s.falloff.min_fraction <= 1.0f);
    }
}

TEST_CASE("loadout: hitscan flag matches the archetype", "[gameplay]") {
    // Hitscan guns: instant ray, no projectile speed.
    for (WeaponClass c : {WeaponClass::Railgun, WeaponClass::Shotgun,
                          WeaponClass::MachineGun}) {
        const WeaponSpec s = weapon_spec(c);
        CHECK(s.hitscan);
        CHECK(s.projectile_speed_mps == Catch::Approx(0.0f));
    }
    // Projectile weapons: travelling bolt/rocket with a positive muzzle speed.
    for (WeaponClass c : {WeaponClass::RocketLauncher, WeaponClass::Plasma}) {
        const WeaponSpec s = weapon_spec(c);
        CHECK_FALSE(s.hitscan);
        CHECK(s.projectile_speed_mps > 0.0f);
    }
}

TEST_CASE("loadout: equip_weapon fills a Weapon ready to fire", "[gameplay]") {
    const WeaponSpec spec = weapon_spec(WeaponClass::Railgun);

    Weapon w{};
    w.cooldown_s = 9.0f;  // dirty pre-state to prove equip arms it
    w.ammo = 3;
    equip_weapon(w, WeaponClass::Railgun);

    CHECK(w.damage == Catch::Approx(spec.damage));
    CHECK(w.fire_interval_s == Catch::Approx(spec.fire_interval_s));
    CHECK(w.hit_radius == Catch::Approx(spec.hit_radius));
    CHECK(w.ammo == spec.max_ammo);   // full magazine
    CHECK(w.cooldown_s == 0.0f);      // armed, ready to fire this instant
}

TEST_CASE("loadout: railgun out-damages the chaingun but fires far slower",
          "[gameplay]") {
    const WeaponSpec rail = weapon_spec(WeaponClass::Railgun);
    const WeaponSpec mg = weapon_spec(WeaponClass::MachineGun);

    // The rail hits far harder per shot.
    CHECK(rail.damage > mg.damage);
    // But the chaingun fires much faster (a strictly smaller interval).
    CHECK(mg.fire_interval_s < rail.fire_interval_s);
    // And carries a deeper belt than the rail's slim magazine.
    CHECK(mg.max_ammo > rail.max_ammo);
}

TEST_CASE("loadout: the shotgun falls off harder than the railgun", "[gameplay]") {
    const WeaponSpec rail = weapon_spec(WeaponClass::Railgun);
    const WeaponSpec sg = weapon_spec(WeaponClass::Shotgun);

    // The concrete relationship chosen: BOTH a lower long-range floor AND a
    // shorter effective range (the shotgun is point-blank only).
    CHECK(sg.falloff.min_fraction < rail.falloff.min_fraction);
    CHECK(sg.falloff.falloff_end_m < rail.falloff.falloff_end_m);
}

TEST_CASE("loadout: out-of-range class returns the default without UB",
          "[gameplay]") {
    // A cast from an unknown integer must not be UB — it returns the documented
    // MachineGun default.
    const WeaponSpec def = weapon_spec(static_cast<WeaponClass>(9999u));
    const WeaponSpec mg = weapon_spec(WeaponClass::MachineGun);

    CHECK(def.damage == Catch::Approx(mg.damage));
    CHECK(def.fire_interval_s == Catch::Approx(mg.fire_interval_s));
    CHECK(def.max_ammo == mg.max_ammo);
    CHECK(def.hit_radius == Catch::Approx(mg.hit_radius));
    CHECK(def.hitscan == mg.hitscan);
}

TEST_CASE("loadout: weapon_spec and equip are deterministic (bit-identical)",
          "[gameplay][determinism]") {
    for (WeaponClass c : kAllClasses) {
        const WeaponSpec a = weapon_spec(c);
        const WeaponSpec b = weapon_spec(c);
        // Exact == on every field — same input, bit-identical spec every call.
        CHECK(a.damage == b.damage);
        CHECK(a.fire_interval_s == b.fire_interval_s);
        CHECK(a.max_ammo == b.max_ammo);
        CHECK(a.hit_radius == b.hit_radius);
        CHECK(a.hitscan == b.hitscan);
        CHECK(a.projectile_speed_mps == b.projectile_speed_mps);
        CHECK(a.falloff.falloff_start_m == b.falloff.falloff_start_m);
        CHECK(a.falloff.falloff_end_m == b.falloff.falloff_end_m);
        CHECK(a.falloff.min_fraction == b.falloff.min_fraction);

        Weapon w1{};
        Weapon w2{};
        equip_weapon(w1, c);
        equip_weapon(w2, c);
        CHECK(w1.damage == w2.damage);
        CHECK(w1.fire_interval_s == w2.fire_interval_s);
        CHECK(w1.cooldown_s == w2.cooldown_s);
        CHECK(w1.ammo == w2.ammo);
        CHECK(w1.hit_radius == w2.hit_radius);
    }
}
