// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/match/Loadout.h — per-class spawn loadouts: the class-based-FPS spawn
// kit. A Loadout binds a player CLASS (Assault / Scout / Heavy) to the gear it
// spawns with — its primary + secondary WeaponClass, starting health, and a
// movement-speed multiplier — and applies that kit to a freshly-spawned pawn.
//
// This is the additive seam between "what a class is" (the immutable spawn
// table) and "a pawn that just spawned" (its ECS Weapon + Health). It composes
// the gameplay arsenal (gameplay::WeaponClass / equip_weapon, the per-weapon
// archetype) with the class layer match cares about. The match lane LINKS
// psynder_gameplay, so it includes "gameplay/WeaponLoadout.h" directly.
//
// Lockstep-safe — class_loadout() is a pure constexpr/switch table (no RNG, no
// transcendentals, no allocation, no branch on platform state): the same
// ClassKit yields a bit-identical Loadout on every peer, every call. apply_*()
// only writes ECS component fields in a fixed order through equip_weapon() and a
// plain f32 store, so on the strict-FP match tick the same inputs produce
// identical pawn state on every platform. 1 world unit = 1 metre; health in hit
// points; speed_mult is a dimensionless scalar on the pawn's base move speed.

#pragma once

#include "gameplay/WeaponLoadout.h"       // WeaponClass, weapon_spec, equip_weapon
#include "gameplay/GameplayComponents.h"  // Weapon, Health
#include "scene/World.h"                   // scene::World, Entity

#include "core/Types.h"

namespace psynder::match {

// The player classes. u32-backed so a kit can ride in POD spawn state / ECS
// components / net events without padding surprises, and so it indexes a flat
// table cleanly. Values are the stable table key; do not renumber (spawn state
// may persist the integer).
enum class ClassKit : u32 {
    Assault = 0,  // the all-rounder: machine-gun primary, baseline health/speed
    Scout   = 1,  // the skirmisher: railgun primary, frail but fast
    Heavy   = 2,  // the bruiser: rocket primary, huge health but slow
};

// Number of distinct ClassKit values — sized so callers can iterate the whole
// roster as `for (u32 i = 0; i < kClassKitCount; ++i)`.
inline constexpr u32 kClassKitCount = 3;

// A class spawn kit. POD / trivially copyable so it can live in a constexpr
// table, a class database, or a net event.
//
//   primary       WeaponClass equipped into the pawn's Weapon on spawn.
//   secondary     WeaponClass carried for a future WeaponInventory hookup; not
//                 equipped yet (the pawn holds one Weapon today).
//   start_health  full health the pawn spawns with — both hp and max_hp (HP).
//   speed_mult    dimensionless multiplier on the pawn's base move speed
//                 (>1 = faster, <1 = slower). Applied by movement code, not here.
struct Loadout {
    gameplay::WeaponClass primary;
    gameplay::WeaponClass secondary;
    f32                   start_health;
    f32                   speed_mult;
};

// The fixed class table: a ClassKit in, its spawn kit out. Pure data (a switch
// over compile-time constants) — deterministic and branch-free of any platform
// state. An out-of-range ClassKit returns the Assault kit (the sane, always-
// available default), never undefined behaviour.
//
//   Assault = { MachineGun, Plasma,         100 HP, 1.00x }  baseline all-rounder
//   Scout   = { Railgun,    Plasma,          75 HP, 1.20x }  glass cannon, fast
//   Heavy   = { RocketLauncher, Shotgun,    150 HP, 0.85x }  tank, slow
//
// Speed mults are ordered Heavy(0.85) < Assault(1.00) < Scout(1.20): the fragile
// scout outruns the baseline, the armoured heavy lumbers behind it.
Loadout class_loadout(ClassKit kit) noexcept;

// Apply a loadout to a freshly-spawned pawn:
//   * if the pawn has a Weapon, equip_weapon(it, lo.primary) — full magazine,
//     armed, the primary's archetype stats;
//   * if the pawn has a Health, set hp = max_hp = lo.start_health (spawn full).
// A no-op for whichever component the pawn lacks (a pawn with neither is left
// untouched). secondary / speed_mult are carried on the Loadout for future
// WeaponInventory + movement hookups; this function does not act on them.
// Deterministic: fixed-order ECS writes, no RNG.
void apply_loadout(scene::World& w, Entity pawn, const Loadout& lo) noexcept;

// Convenience composition: apply_loadout(w, pawn, class_loadout(kit)).
void apply_class(scene::World& w, Entity pawn, ClassKit kit) noexcept;

}  // namespace psynder::match
