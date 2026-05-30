// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/gameplay/WeaponLoadout.cpp — see WeaponLoadout.h. The archetype table
// and the equip factory. Pure deterministic data; owns and mutates no global
// state. Every number below is a metric, Quake3-class value (1 unit = 1 m,
// damage in HP, intervals in seconds, speeds in m/s).

#include "gameplay/WeaponLoadout.h"

namespace psynder::gameplay {

namespace {

// The canonical arsenal, one designed spec per class. Kept in this anonymous
// namespace as plain constexpr structs (not a std::array indexed by the enum) so
// the switch in weapon_spec() is the single, explicit, out-of-range-safe lookup.
//
// Tuning rationale (playable map scales are ~10-100 m, so falloff ramps sit in
// that window; the falloff profiles are the canonical curves from RangedDamage.h):
//
//   Railgun        80 HP @ 1.5 s, hitscan, kRifleFalloff, 10 ammo, r=0.5.
//                  The reward weapon: a slow, punishing instant-hit rail that
//                  still bites across a large outdoor engagement (gentle rifle
//                  falloff to 60% at 90 m), but a tiny magazine and long refire.
//   RocketLauncher 100 HP @ 0.8 s, projectile @ 30 m/s, kNoFalloff, 10 ammo, r=0.5.
//                  The heavy: highest direct damage, a slow visible rocket the
//                  enemy can dodge; direct hit does full damage at any range
//                  (radius splash is modelled elsewhere, hence kNoFalloff).
//   Shotgun        60 HP @ 1.0 s, hitscan, kShotgunFalloff, 10 ammo, r=0.5.
//                  The brawler: devastating point-blank (full to 6 m) collapsing
//                  to 15% of base by 18 m — near-useless across a courtyard.
//   MachineGun     7 HP @ 0.1 s, hitscan, kPistolFalloff, 100 ammo, r=0.5.
//                  The workhorse / default: low per-shot but a 10 Hz stream and a
//                  deep 100-round belt; medium falloff (full to 12 m, 45% by 35 m).
//   Plasma         20 HP @ 0.1 s, projectile @ 40 m/s, kNoFalloff, 50 ammo, r=0.4.
//                  The spam weapon: a fast wall of fast bolts; lower per-bolt than
//                  the rail, mobile, no distance falloff on a direct bolt hit, with
//                  a slightly tighter hitbox (0.4 m) than the hitscan guns.
constexpr WeaponSpec kRailgun{
    /*damage=*/80.0f, /*fire_interval_s=*/1.5f, /*max_ammo=*/10,
    /*hit_radius=*/0.5f, /*hitscan=*/true, /*projectile_speed_mps=*/0.0f,
    /*falloff=*/kRifleFalloff};

constexpr WeaponSpec kRocketLauncher{
    /*damage=*/100.0f, /*fire_interval_s=*/0.8f, /*max_ammo=*/10,
    /*hit_radius=*/0.5f, /*hitscan=*/false, /*projectile_speed_mps=*/30.0f,
    /*falloff=*/kNoFalloff};

constexpr WeaponSpec kShotgun{
    /*damage=*/60.0f, /*fire_interval_s=*/1.0f, /*max_ammo=*/10,
    /*hit_radius=*/0.5f, /*hitscan=*/true, /*projectile_speed_mps=*/0.0f,
    /*falloff=*/kShotgunFalloff};

constexpr WeaponSpec kMachineGun{
    /*damage=*/7.0f, /*fire_interval_s=*/0.1f, /*max_ammo=*/100,
    /*hit_radius=*/0.5f, /*hitscan=*/true, /*projectile_speed_mps=*/0.0f,
    /*falloff=*/kPistolFalloff};

constexpr WeaponSpec kPlasma{
    /*damage=*/20.0f, /*fire_interval_s=*/0.1f, /*max_ammo=*/50,
    /*hit_radius=*/0.4f, /*hitscan=*/false, /*projectile_speed_mps=*/40.0f,
    /*falloff=*/kNoFalloff};

}  // namespace

WeaponSpec weapon_spec(WeaponClass c) noexcept {
    switch (c) {
        case WeaponClass::Railgun:        return kRailgun;
        case WeaponClass::RocketLauncher: return kRocketLauncher;
        case WeaponClass::Shotgun:        return kShotgun;
        case WeaponClass::MachineGun:     return kMachineGun;
        case WeaponClass::Plasma:         return kPlasma;
    }
    // Out-of-range (a cast from an unknown integer): fall back to the MachineGun,
    // a sane always-available default — never undefined behaviour.
    return kMachineGun;
}

void equip_weapon(Weapon& w, WeaponClass c) noexcept {
    const WeaponSpec s = weapon_spec(c);
    // Copy the fields the shared 20-byte Weapon POD carries; the hitscan flag,
    // projectile speed, and falloff stay on the spec (looked up by the fire code).
    w.damage          = s.damage;
    w.fire_interval_s = s.fire_interval_s;
    w.cooldown_s      = 0.0f;        // armed: ready to fire immediately
    w.ammo            = s.max_ammo;  // full magazine
    w.hit_radius      = s.hit_radius;
}

}  // namespace psynder::gameplay
