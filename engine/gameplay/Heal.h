// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/gameplay/Heal.h — deterministic healing: instant medkit + heal-over-time.
//
// The active-restore complement to Powerup.h's tick_regeneration (which heals
// every entity carrying an active Regeneration powerup). Here a medkit pickup or
// ability calls apply_heal for an instant restore, grant_hot attaches a timed
// heal-over-time buff to one entity, and tick_heals advances every such buff on
// the authoritative lockstep tick. Like Damage.{h,cpp}: POD components, no RNG,
// stable ascending entity-id processing order, no per-tick heap alloc beyond a
// reused gather buffer. Pure algebra => bit-identical for identical inputs.

#pragma once

#include "scene/World.h"  // PSYNDER_COMPONENT (registers a POD component id)

#include "core/Types.h"

#include <vector>

namespace psynder::gameplay {

// Add `amount` of health to entity `e` (needs a Health component), clamped so
// that hp never exceeds `over_max_cap`. Pass over_max_cap == Health.max_hp for a
// normal heal that tops off at full, or a larger value to allow overhealing past
// max (Quake Regeneration tops out at 200; a "mega health" pickup does the same).
//
// Returns the actual amount healed: 0 if `e` has no Health, if hp is already at
// or above the cap, or if `amount <= 0`.
//
// Dead-entity policy: an entity at hp <= 0 is considered dead and is NOT revived
// by a heal — apply_heal returns 0 for it (mirrors apply_damage in Damage.cpp,
// which refuses to act on hp <= 0). Revival is a deliberate, separate act (a
// respawn / clearing the Dead tag), never an accidental side effect of healing.
f32 apply_heal(scene::World& w, Entity e, f32 amount, f32 over_max_cap) noexcept;

// A heal-over-time buff: restores `rate_per_s` health per second for the next
// `remaining_s` seconds, each tick clamped to `over_max_cap` (the same overheal
// ceiling apply_heal uses). POD; replicated by the netcode, ticked by tick_heals.
PSYNDER_COMPONENT(HealOverTime) {
    f32 rate_per_s;    // health restored per second while active
    f32 remaining_s;   // seconds of buff left; expires (removed) at <= 0
    f32 over_max_cap;  // ceiling hp may reach via this buff (>= max_hp to overheal)
};
static_assert(sizeof(HealOverTime) == 12, "HealOverTime layout frozen (3 f32)");

// Attach (or refresh) a HealOverTime buff on `e`. If `e` already carries one it
// is refreshed to the STRONGER buff: the larger rate_per_s, the longer remaining
// duration, and the higher over_max_cap are each kept independently — so a weaker
// or shorter top-up never downgrades an active buff. Negative inputs clamp to 0.
// Granting a zero/negative duration on an entity with no existing buff is a no-op.
void grant_hot(scene::World& w, Entity e, f32 rate_per_s, f32 duration_s,
               f32 over_max_cap) noexcept;

// Advance every HealOverTime buff by `dt_s`: heal rate_per_s*dt_s (through the
// same cap logic as apply_heal) and decrement remaining_s. When remaining_s <= 0
// the buff has expired and its component is REMOVED. Deterministic: timers tick
// in place during the read walk; healing + expiry are then applied in ascending
// entity-id order so the result is independent of chunk/storage order. Reuses
// `scratch` across ticks (no per-tick heap alloc). No RNG.
void tick_heals(scene::World& w, f32 dt_s, std::vector<Entity>& scratch) noexcept;

}  // namespace psynder::gameplay
