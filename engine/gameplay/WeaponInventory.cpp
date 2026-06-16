// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/gameplay/WeaponInventory.cpp — see WeaponInventory.h. Pure integer/bit
// logic over the owned_mask plus the per-class reserve ammo. Deterministic,
// lockstep-safe: no RNG, no transcendentals, no allocation. Cycling walks the
// fixed table order (WeaponClass 0..kWeaponClassCount-1) so "next/prev owned" is
// bit-identical on every peer.

#include "gameplay/WeaponInventory.h"

namespace psynder::gameplay {

namespace {

// 1 << index of WeaponClass `c`. The enum is the stable, contiguous table key
// [0, kWeaponClassCount); only in-range classes occupy a bit.
constexpr u32 class_bit(WeaponClass c) noexcept {
    return 1u << static_cast<u32>(c);
}

// True iff `c` is a real table index (defends give/has against a cast from an
// out-of-range integer — keeps shifts and array indexing in bounds, no UB).
constexpr bool in_range(WeaponClass c) noexcept {
    return static_cast<u32>(c) < kWeaponClassCount;
}

}  // namespace

void give_weapon(WeaponInventory& inv, WeaponClass c) noexcept {
    if (!in_range(c)) return;
    const u32 bit = class_bit(c);
    // First acquisition only: stock the reserve from the archetype. A re-pickup
    // of an owned class must NOT refill — guard ammo on the not-yet-owned check.
    if ((inv.owned_mask & bit) == 0u) {
        inv.owned_mask |= bit;
        inv.ammo[static_cast<u32>(c)] = weapon_spec(c).max_ammo;
    }
}

bool has_weapon(const WeaponInventory& inv, WeaponClass c) noexcept {
    if (!in_range(c)) return false;
    return (inv.owned_mask & class_bit(c)) != 0u;
}

bool switch_to(WeaponInventory& inv, WeaponClass c) noexcept {
    if (!has_weapon(inv, c)) return false;
    inv.active = static_cast<u32>(c);
    return true;
}

bool cycle_next(WeaponInventory& inv) noexcept {
    // Need at least two owned classes for "next" to mean anything. popcount on a
    // <= 5-bit mask via a tiny deterministic loop (no <bit> dependency, no UB).
    u32 owned = inv.owned_mask;
    u32 count = 0u;
    for (u32 m = owned; m != 0u; m &= (m - 1u)) ++count;
    if (count <= 1u) return false;

    // Walk forward from active+1, wrapping, to the next set bit. At least one of
    // the kWeaponClassCount-1 other slots is owned, so this always lands.
    u32 i = inv.active;
    for (u32 step = 0u; step < kWeaponClassCount; ++step) {
        i = (i + 1u) % kWeaponClassCount;
        if ((owned & (1u << i)) != 0u) {
            inv.active = i;
            return true;
        }
    }
    return false;  // unreachable with count >= 2
}

bool cycle_prev(WeaponInventory& inv) noexcept {
    u32 owned = inv.owned_mask;
    u32 count = 0u;
    for (u32 m = owned; m != 0u; m &= (m - 1u)) ++count;
    if (count <= 1u) return false;

    // Walk backward from active-1, wrapping, to the previous set bit.
    u32 i = inv.active;
    for (u32 step = 0u; step < kWeaponClassCount; ++step) {
        i = (i + kWeaponClassCount - 1u) % kWeaponClassCount;
        if ((owned & (1u << i)) != 0u) {
            inv.active = i;
            return true;
        }
    }
    return false;  // unreachable with count >= 2
}

WeaponClass active_weapon(const WeaponInventory& inv) noexcept {
    return static_cast<WeaponClass>(inv.active);
}

void equip_active(scene::World& w, Entity e) noexcept {
    WeaponInventory* inv = w.get<WeaponInventory>(e);
    if (inv == nullptr) return;
    Weapon* weapon = w.get<Weapon>(e);
    if (weapon == nullptr) return;

    const WeaponClass c = active_weapon(*inv);
    // Copy the archetype fields (damage / fire_interval_s / hit_radius, arm it),
    // then overwrite the freshly-filled magazine with the held reserve so a
    // switch restores the rounds you had rather than refilling.
    equip_weapon(*weapon, c);
    if (in_range(c)) {
        weapon->ammo = inv->ammo[static_cast<u32>(c)];
    }
}

}  // namespace psynder::gameplay
