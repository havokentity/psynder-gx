// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/match/Loadout.cpp — see Loadout.h. The class spawn table and the apply
// factory. Pure deterministic data; owns and mutates no global state. Every
// number below is a metric, Quake3-class value (health in HP, speed_mult a
// dimensionless scalar on the pawn's base move speed).

#include "match/Loadout.h"

namespace psynder::match {

using gameplay::Health;
using gameplay::Weapon;
using gameplay::WeaponClass;

namespace {

// The canonical roster, one designed kit per class. Kept in this anonymous
// namespace as plain constexpr structs (not a std::array indexed by the enum) so
// the switch in class_loadout() is the single, explicit, out-of-range-safe lookup.
//
// Tuning rationale (the per-weapon stats live in gameplay/WeaponLoadout.cpp; the
// class layer picks weapons + sets the survivability/mobility envelope):
//
//   Assault  MachineGun + Plasma, 100 HP, 1.00x. The baseline all-rounder: the
//            10 Hz machine-gun workhorse, a full 100-HP health bar, and unmodified
//            move speed — the yardstick every other class is tuned against.
//   Scout    Railgun + Plasma, 75 HP, 1.20x. The glass cannon: a punishing
//            instant-hit rail rewards aim, but only 75 HP — it dies fast, so it
//            moves 20% faster to reposition and pick fights on its terms.
//   Heavy    RocketLauncher + Shotgun, 150 HP, 0.85x. The bruiser: the highest-
//            direct-damage rocket plus a point-blank shotgun sidearm, a huge
//            150-HP bar, but 15% slower — area denial that cannot disengage.
constexpr Loadout kAssault{
    /*primary=*/WeaponClass::MachineGun, /*secondary=*/WeaponClass::Plasma,
    /*start_health=*/100.0f, /*speed_mult=*/1.0f};

constexpr Loadout kScout{
    /*primary=*/WeaponClass::Railgun, /*secondary=*/WeaponClass::Plasma,
    /*start_health=*/75.0f, /*speed_mult=*/1.2f};

constexpr Loadout kHeavy{
    /*primary=*/WeaponClass::RocketLauncher, /*secondary=*/WeaponClass::Shotgun,
    /*start_health=*/150.0f, /*speed_mult=*/0.85f};

}  // namespace

Loadout class_loadout(ClassKit kit) noexcept {
    switch (kit) {
        case ClassKit::Assault: return kAssault;
        case ClassKit::Scout:   return kScout;
        case ClassKit::Heavy:   return kHeavy;
    }
    // Out-of-range (a cast from an unknown integer): fall back to the Assault
    // kit, a sane always-available default — never undefined behaviour.
    return kAssault;
}

void apply_loadout(scene::World& w, Entity pawn, const Loadout& lo) noexcept {
    // Fixed write order: weapon first, then health. Each store is guarded by the
    // component's presence (get<> returns nullptr when absent), so a pawn missing
    // either component is a safe partial no-op.
    if (Weapon* wp = w.get<Weapon>(pawn)) {
        equip_weapon(*wp, lo.primary);  // full magazine, armed, primary's stats
    }
    if (Health* hp = w.get<Health>(pawn)) {
        hp->max_hp = lo.start_health;  // spawn at the class's full health bar
        hp->hp     = lo.start_health;
    }
    // secondary / speed_mult are carried on the Loadout for a future
    // WeaponInventory + movement hookup; nothing to apply here yet.
}

void apply_class(scene::World& w, Entity pawn, ClassKit kit) noexcept {
    apply_loadout(w, pawn, class_loadout(kit));
}

}  // namespace psynder::match
