// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/gameplay/WeaponLoadout.h — canonical FPS weapon arsenal as data.
//
// A Quake3-class roster of weapon archetypes expressed as a fixed, deterministic
// data table plus a factory that equips the `Weapon` ECS component from a spec.
// This is the additive seam between "what a weapon is" (the immutable archetype:
// damage, fire rate, ammo, hitscan-vs-projectile, falloff curve) and "a weapon a
// shooter holds" (the mutable `Weapon` component the fire code mutates each tick).
//
// Lockstep-safe — the table is pure constexpr/switch data, no RNG, no
// transcendentals, no allocation, no branches on platform state. The same
// WeaponClass yields a bit-identical WeaponSpec on every peer, every call; the
// gameplay lane is strict-FP, so equip_weapon() writes identical f32/i32 fields
// everywhere. 1 world unit = 1 metre; damage in hit points, intervals in seconds,
// speeds in metres/second.
//
// The spec carries the hitscan flag, projectile speed, and falloff profile; the
// fire code (Weapons.*) consults those off the spec. equip_weapon() only writes
// the fields that live on the shared `Weapon` component — see its doc below.

#pragma once

#include "core/Types.h"
#include "gameplay/GameplayComponents.h"  // Weapon component
#include "gameplay/RangedDamage.h"         // FalloffProfile + canonical profiles

namespace psynder::gameplay {

// The canonical weapon archetypes — a Quake3-class arsenal. u32-backed so the
// class can ride in POD loadout state / ECS components / net events without
// padding surprises, and so it indexes a flat table cleanly. Values are the
// stable table key; do not renumber (loadout state may persist the integer).
enum class WeaponClass : u32 {
    Railgun = 0,        // hitscan rail: slow, hard-hitting, long range
    RocketLauncher = 1, // projectile: high direct damage, splash handled elsewhere
    Shotgun = 2,        // hitscan spread: brutal point-blank, dead past mid-room
    MachineGun = 3,     // hitscan chaingun: low per-shot, very high rate, big mag
    Plasma = 4,         // projectile: fast-firing, mobile, low-medium per-bolt
};

// Number of distinct WeaponClass values — sized so callers can iterate the whole
// arsenal as `for (u32 i = 0; i < kWeaponClassCount; ++i)`.
inline constexpr u32 kWeaponClassCount = 5;

// An immutable weapon archetype. POD / trivially copyable so it can live in a
// constexpr table, a stats database, or be copied into an ECS `Weapon`.
//
//   damage              base hit points per shot (pre-falloff, pre-hitbox).
//   fire_interval_s     seconds between shots — smaller = faster fire.
//   max_ammo            rounds in a full magazine; < 0 means infinite (matches the
//                       Weapon component's ammo < 0 = infinite convention).
//   hit_radius          target hitbox sphere the hitscan ray tests against (m).
//   hitscan             true  => instant-hit ray (projectile_speed_mps unused/0),
//                       false => a travelling projectile (use projectile_speed_mps).
//   projectile_speed_mps  muzzle speed of the projectile in m/s; 0 for hitscan.
//   falloff             distance-falloff curve (reuses the canonical profiles in
//                       RangedDamage.h); splash weapons use kNoFalloff.
struct WeaponSpec {
    f32            damage;
    f32            fire_interval_s;
    i32            max_ammo;
    f32            hit_radius;
    bool           hitscan;
    f32            projectile_speed_mps;
    FalloffProfile falloff;
};

// The fixed archetype table: a WeaponClass in, its metric spec out. Pure data
// (a switch over compile-time constants) — deterministic and branch-free of any
// platform state. An out-of-range WeaponClass returns the MachineGun spec (a
// sane, always-available default), never undefined behaviour.
WeaponSpec weapon_spec(WeaponClass c) noexcept;

// Equip a shooter's `Weapon` component from an archetype: copies the shared
// fields (damage, fire_interval_s, hit_radius), fills the magazine
// (ammo = max_ammo), and arms the weapon ready-to-fire (cooldown_s = 0). The
// hitscan flag, projectile speed, and falloff profile stay on the spec — the
// fire code looks those up by WeaponClass — because the `Weapon` component is the
// frozen 20-byte POD shared by every weapon and does not carry them.
void equip_weapon(Weapon& w, WeaponClass c) noexcept;

}  // namespace psynder::gameplay
