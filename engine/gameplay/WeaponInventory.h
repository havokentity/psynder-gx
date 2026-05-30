// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/gameplay/WeaponInventory.h — a multi-weapon inventory + switching.
//
// The mutable per-player loadout state that sits one layer above the immutable
// archetype table (WeaponLoadout.h): which WeaponClass archetypes a shooter
// *owns*, how much *reserve* ammo it is holding for each, and which one is
// *active* right now. This is the seam between "the arsenal as data" and "the
// frozen 20-byte Weapon component the fire code mutates" — equip_active()
// projects the active class into that Weapon each time the player switches.
//
// POD component, pure integer/bit logic: no RNG, no transcendentals, no
// allocation, no branches on platform state. The gameplay lane is strict-FP and
// lockstep-deterministic — give/switch/cycle are bit operations over the
// owned_mask, so the same sequence of inputs yields a bit-identical inventory on
// every peer, every call. Cycling order is the fixed table order (WeaponClass
// 0..kWeaponClassCount-1), so "next owned weapon" is deterministic everywhere.
//
// owned_mask: bit i set  <=> the player owns WeaponClass i.
// active:     the selected WeaponClass index (only meaningful once owned).
// ammo[i]:    reserve ammo the player holds for WeaponClass i (the magazine the
//             active weapon fires from lives on the Weapon component; switching
//             restores ammo[active] back into Weapon.ammo via equip_active()).

#pragma once

#include "scene/World.h"  // scene::World, Entity, PSYNDER_COMPONENT

#include "gameplay/WeaponLoadout.h"  // WeaponClass, kWeaponClassCount, weapon_spec, equip_weapon

#include "core/Types.h"

namespace psynder::gameplay {

// A player's multi-weapon inventory. POD / trivially copyable so it rides in an
// ECS chunk and replicates over the wire without padding surprises.
//
//   owned_mask  bitset over WeaponClass: bit i => owns class i.
//   active      the currently selected WeaponClass index.
//   ammo[i]     reserve ammo held for class i (set on first pickup; preserved
//               across weapon switches so swapping back keeps your rounds).
PSYNDER_COMPONENT(WeaponInventory) {
    u32 owned_mask;
    u32 active;
    i32 ammo[kWeaponClassCount];
};
// 4 (owned_mask) + 4 (active) + 4*kWeaponClassCount (ammo) = 28 bytes at
// kWeaponClassCount == 5; all u32/i32, 4-byte aligned, no padding.
static_assert(sizeof(WeaponInventory) == 8 + 4 * kWeaponClassCount,
              "WeaponInventory layout frozen");
static_assert(sizeof(WeaponInventory) == 28,
              "WeaponInventory layout frozen (kWeaponClassCount == 5)");

// Grant WeaponClass `c`: set its owned bit and, ONLY on first acquisition, fill
// its reserve from the archetype (ammo[c] = weapon_spec(c).max_ammo). A re-pickup
// of an already-owned class is a deliberate no-op on ammo — picking the gun up
// again does NOT refill the reserve (refills come from ammo pickups, not the
// weapon). Out-of-range `c` is ignored. Does not change `active`.
void give_weapon(WeaponInventory& inv, WeaponClass c) noexcept;

// True iff the inventory owns WeaponClass `c` (its owned bit is set).
bool has_weapon(const WeaponInventory& inv, WeaponClass c) noexcept;

// Select WeaponClass `c` as active iff it is owned. Returns true on success;
// returns false and leaves `active` unchanged if `c` is not owned.
bool switch_to(WeaponInventory& inv, WeaponClass c) noexcept;

// Advance `active` to the next OWNED class in ascending table order, wrapping
// past the end. Deterministic. Returns false (and leaves `active` unchanged) if
// the inventory owns 0 or 1 weapons (nothing to cycle to).
bool cycle_next(WeaponInventory& inv) noexcept;

// As cycle_next, but moves to the previous OWNED class (descending table order,
// wrapping past the start). Deterministic. Returns false with <= 1 owned.
bool cycle_prev(WeaponInventory& inv) noexcept;

// The currently selected WeaponClass (active reinterpreted as the enum).
WeaponClass active_weapon(const WeaponInventory& inv) noexcept;

// Equip the active weapon into the entity's held Weapon: read the entity's
// WeaponInventory, equip_weapon(its Weapon, active class) to copy the archetype
// fields, then restore that class's *reserve* ammo (inv.ammo[active]) into
// Weapon.ammo — so switching back to a weapon keeps the rounds you had, rather
// than re-filling the magazine. No-op if the entity lacks a WeaponInventory or a
// Weapon component. Deterministic; pure component read/write, no alloc.
void equip_active(scene::World& w, Entity e) noexcept;

}  // namespace psynder::gameplay
