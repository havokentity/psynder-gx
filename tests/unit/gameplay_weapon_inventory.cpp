// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/gameplay_weapon_inventory.cpp — the multi-weapon inventory:
// give sets ownership + reserve (and a re-give never refills), has reports
// ownership, switch_to only lands on owned classes, cycle_next/prev skips
// unowned classes and wraps deterministically (false with <= 1 owned), and
// equip_active projects the active archetype into the held Weapon while
// restoring that class's reserve ammo. All bit/integer logic — bit-identical
// on every peer.

#include "gameplay/WeaponInventory.h"
#include "gameplay/WeaponLoadout.h"
#include "gameplay/GameplayComponents.h"

#include "scene/World.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace psynder;
using namespace psynder::gameplay;

TEST_CASE("inventory: give sets ownership and stocks reserve once", "[gameplay]") {
    WeaponInventory inv{};  // owns nothing, all reserves zero

    CHECK_FALSE(has_weapon(inv, WeaponClass::Railgun));

    give_weapon(inv, WeaponClass::Railgun);
    CHECK(has_weapon(inv, WeaponClass::Railgun));
    // First acquisition fills the reserve from the archetype.
    CHECK(inv.ammo[static_cast<u32>(WeaponClass::Railgun)] ==
          weapon_spec(WeaponClass::Railgun).max_ammo);

    // A re-pickup of an already-owned class must NOT refill the reserve.
    inv.ammo[static_cast<u32>(WeaponClass::Railgun)] = 2;  // spent down
    give_weapon(inv, WeaponClass::Railgun);
    CHECK(has_weapon(inv, WeaponClass::Railgun));
    CHECK(inv.ammo[static_cast<u32>(WeaponClass::Railgun)] == 2);  // untouched
}

TEST_CASE("inventory: has_weapon reflects only owned classes", "[gameplay]") {
    WeaponInventory inv{};
    give_weapon(inv, WeaponClass::Shotgun);

    CHECK(has_weapon(inv, WeaponClass::Shotgun));
    CHECK_FALSE(has_weapon(inv, WeaponClass::Railgun));
    CHECK_FALSE(has_weapon(inv, WeaponClass::RocketLauncher));
    CHECK_FALSE(has_weapon(inv, WeaponClass::MachineGun));
    CHECK_FALSE(has_weapon(inv, WeaponClass::Plasma));
}

TEST_CASE("inventory: switch_to only lands on an owned class", "[gameplay]") {
    WeaponInventory inv{};
    give_weapon(inv, WeaponClass::Railgun);
    give_weapon(inv, WeaponClass::Plasma);

    // Owned: switch succeeds and active follows.
    CHECK(switch_to(inv, WeaponClass::Plasma));
    CHECK(active_weapon(inv) == WeaponClass::Plasma);

    // Not owned: switch fails and active is unchanged.
    CHECK_FALSE(switch_to(inv, WeaponClass::Shotgun));
    CHECK(active_weapon(inv) == WeaponClass::Plasma);
}

TEST_CASE("inventory: cycle_next visits exactly the owned classes in order",
          "[gameplay]") {
    // Own classes 0, 2, 4 (Railgun, Shotgun, Plasma) — cycling must skip the
    // unowned 1 and 3 and wrap, visiting exactly {0, 2, 4} in ascending order.
    WeaponInventory inv{};
    give_weapon(inv, WeaponClass::Railgun);   // 0
    give_weapon(inv, WeaponClass::Shotgun);   // 2
    give_weapon(inv, WeaponClass::Plasma);    // 4

    REQUIRE(switch_to(inv, WeaponClass::Railgun));
    CHECK(active_weapon(inv) == WeaponClass::Railgun);

    CHECK(cycle_next(inv));
    CHECK(active_weapon(inv) == WeaponClass::Shotgun);  // skipped 1
    CHECK(cycle_next(inv));
    CHECK(active_weapon(inv) == WeaponClass::Plasma);   // skipped 3
    CHECK(cycle_next(inv));
    CHECK(active_weapon(inv) == WeaponClass::Railgun);  // wrapped past the end
}

TEST_CASE("inventory: cycle_prev is the deterministic reverse walk", "[gameplay]") {
    WeaponInventory inv{};
    give_weapon(inv, WeaponClass::Railgun);   // 0
    give_weapon(inv, WeaponClass::Shotgun);   // 2
    give_weapon(inv, WeaponClass::Plasma);    // 4

    REQUIRE(switch_to(inv, WeaponClass::Railgun));

    CHECK(cycle_prev(inv));
    CHECK(active_weapon(inv) == WeaponClass::Plasma);   // wrapped past the start
    CHECK(cycle_prev(inv));
    CHECK(active_weapon(inv) == WeaponClass::Shotgun);
    CHECK(cycle_prev(inv));
    CHECK(active_weapon(inv) == WeaponClass::Railgun);
}

TEST_CASE("inventory: cycling fails with zero or one owned weapon", "[gameplay]") {
    WeaponInventory empty{};
    CHECK_FALSE(cycle_next(empty));
    CHECK_FALSE(cycle_prev(empty));

    WeaponInventory one{};
    give_weapon(one, WeaponClass::MachineGun);
    REQUIRE(switch_to(one, WeaponClass::MachineGun));
    CHECK_FALSE(cycle_next(one));  // nothing to cycle to
    CHECK_FALSE(cycle_prev(one));
    CHECK(active_weapon(one) == WeaponClass::MachineGun);  // unchanged
}

TEST_CASE("inventory: equip_active writes the active archetype into the Weapon",
          "[gameplay]") {
    scene::World w;
    const Entity e = w.create();

    WeaponInventory inv{};
    give_weapon(inv, WeaponClass::Railgun);
    give_weapon(inv, WeaponClass::Plasma);
    REQUIRE(switch_to(inv, WeaponClass::Plasma));
    w.add(e, inv);
    w.add(e, Weapon{});

    equip_active(w, e);

    const WeaponSpec plasma = weapon_spec(WeaponClass::Plasma);
    const Weapon* held = w.get<Weapon>(e);
    REQUIRE(held != nullptr);
    CHECK(held->damage == Catch::Approx(plasma.damage));
    CHECK(held->fire_interval_s == Catch::Approx(plasma.fire_interval_s));
    CHECK(held->hit_radius == Catch::Approx(plasma.hit_radius));
    CHECK(held->cooldown_s == Catch::Approx(0.0f));  // armed, ready to fire
}

TEST_CASE("inventory: equip_active restores the held reserve ammo on switch",
          "[gameplay]") {
    scene::World w;
    const Entity e = w.create();

    WeaponInventory inv{};
    give_weapon(inv, WeaponClass::MachineGun);  // reserve = full belt
    // Spend the machine gun down to a partial reserve.
    inv.ammo[static_cast<u32>(WeaponClass::MachineGun)] = 37;
    REQUIRE(switch_to(inv, WeaponClass::MachineGun));
    w.add(e, inv);
    w.add(e, Weapon{});

    equip_active(w, e);

    const Weapon* held = w.get<Weapon>(e);
    REQUIRE(held != nullptr);
    // Not the full magazine (max_ammo) — the *held* reserve is restored.
    CHECK(held->ammo == 37);
    CHECK(held->ammo != weapon_spec(WeaponClass::MachineGun).max_ammo);
}

TEST_CASE("inventory: equip_active is a no-op without the components", "[gameplay]") {
    scene::World w;

    // Entity with neither component: equip_active must not crash or add anything.
    const Entity bare = w.create();
    equip_active(w, bare);
    CHECK(w.get<Weapon>(bare) == nullptr);

    // Entity with an inventory but no Weapon: still a no-op (nothing to write to).
    const Entity no_weapon = w.create();
    WeaponInventory inv{};
    give_weapon(inv, WeaponClass::Railgun);
    REQUIRE(switch_to(inv, WeaponClass::Railgun));
    w.add(no_weapon, inv);
    equip_active(w, no_weapon);
    CHECK(w.get<Weapon>(no_weapon) == nullptr);
}

TEST_CASE("inventory: give and cycle are deterministic (bit-identical)",
          "[gameplay][determinism]") {
    // Two inventories fed the identical input sequence must be bit-identical
    // field-for-field at every step — pure integer/bit logic, no RNG.
    WeaponInventory a{};
    WeaponInventory b{};

    const WeaponClass seq[] = {WeaponClass::Railgun, WeaponClass::Shotgun,
                               WeaponClass::Plasma, WeaponClass::Railgun};
    for (WeaponClass c : seq) {
        give_weapon(a, c);
        give_weapon(b, c);
    }
    CHECK(a.owned_mask == b.owned_mask);
    for (u32 i = 0; i < kWeaponClassCount; ++i) {
        CHECK(a.ammo[i] == b.ammo[i]);
    }

    REQUIRE(switch_to(a, WeaponClass::Railgun));
    REQUIRE(switch_to(b, WeaponClass::Railgun));
    for (u32 step = 0; step < 7; ++step) {
        const bool ra = cycle_next(a);
        const bool rb = cycle_next(b);
        CHECK(ra == rb);
        CHECK(a.active == b.active);
    }
}
