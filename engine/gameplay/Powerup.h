// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/gameplay/Powerup.h — timed Quake3-style status effects a player carries.
//
// A Powerups component holds a per-kind countdown (QuadDamage, Haste,
// Regeneration, BattleSuit). Systems tick the timers down each authoritative
// frame; queries (damage multipliers, has_powerup) read them. Deterministic:
// pure algebra, stable entity-id processing order, no RNG, no per-frame heap
// alloc in the tick. Mirrors the POD/system split of Damage.{h,cpp} and
// Pickups.{h,cpp}.

#pragma once

#include "scene/World.h"  // PSYNDER_COMPONENT (registers a POD component id)

#include "core/Types.h"

namespace psynder::gameplay {

// The carryable timed powerups (Quake3 staples). Stored as the index into a
// Powerups timer array; values are stable so they double as wire/save ids.
enum class PowerupKind : u32 {
    QuadDamage = 0,    // outgoing damage x4 while active
    Haste = 1,         // faster fire/move while active (consumed by movement/fire)
    Regeneration = 2,  // heals over time, overhealing past max (see tick_regeneration)
    BattleSuit = 3,    // halves incoming damage while active
};

// Number of distinct powerup kinds (size of the per-entity timer array).
inline constexpr u32 kPowerupKindCount = 4;

// Per-player powerup state: seconds remaining for each kind (0 == inactive).
// One timer per PowerupKind, indexed by its enum value. POD; replicated by the
// netcode and mutated by the powerup systems.
PSYNDER_COMPONENT(Powerups) {
    f32 time_left[kPowerupKindCount];  // seconds remaining per kind; 0 = inactive
};
static_assert(sizeof(Powerups) == sizeof(f32) * kPowerupKindCount,
              "Powerups layout frozen (one f32 timer per kind)");
static_assert(sizeof(Powerups) == 16, "Powerups is 4 f32 timers == 16 bytes");

// Grant (or refresh) powerup `k` for `duration_s` seconds. Refreshes to the
// longer of the current and requested remaining time, so re-grabbing a powerup
// never shortens an active one. Deterministic; clamps negatives to 0.
void grant_powerup(Powerups& p, PowerupKind k, f32 duration_s) noexcept;

// True iff powerup `k` is currently active (time_left[k] > 0).
bool has_powerup(const Powerups& p, PowerupKind k) noexcept;

// Decrement every Powerups component's timers by `dt_seconds`, clamping at 0
// (expired). Order-independent: each entity's timers are independent, so chunk
// iteration order does not affect the result. Deterministic; no RNG.
void tick_powerups(scene::World& w, f32 dt_seconds);

// Outgoing-damage multiplier from a player's active powerups: 4.0 while
// QuadDamage is active, else 1.0. A future fire path multiplies dealt damage by
// this before calling apply_damage. Pure function of the component; no side
// effects.
f32 damage_multiplier(const Powerups& p) noexcept;

// Incoming-damage multiplier from a player's active powerups: 0.5 while
// BattleSuit is active, else 1.0. A future damage path multiplies incoming
// damage by this. Pure function of the component; no side effects.
f32 incoming_damage_multiplier(const Powerups& p) noexcept;

// Heal every entity that has both Powerups (Regeneration active) and Health, by
// `hp_per_s * dt_seconds`, up to `over_max_cap` (the overheal ceiling — Quake's
// Regeneration overheals to 200). Never reduces hp already above the cap; only
// heals from below it. Deterministic: timers are read in place; healing is
// applied in ascending entity-id order so the result is storage-order
// independent. No RNG, no per-tick heap alloc.
void tick_regeneration(scene::World& w, f32 dt_seconds, f32 hp_per_s,
                       f32 over_max_cap) noexcept;

}  // namespace psynder::gameplay
