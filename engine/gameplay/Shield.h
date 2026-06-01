// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/gameplay/Shield.h — a regenerating energy shield (Halo-style).
//
// A Shield component soaks incoming damage before Health: a hit drains the
// shield first and only the OVERFLOW (damage beyond the shield) reaches Health
// (routed through gameplay::damage_credited so scoreboard credit still works).
// Taking a hit pauses recharge for recharge_delay_s; once that pause elapses the
// shield regenerates at recharge_rate (points/second) up to max_shield.
//
// On the authoritative deterministic tick: no RNG, stable ascending-entity-id
// processing order, no per-tick heap alloc. Mirrors the POD/system split of
// Damage.{h,cpp} and Powerup.{h,cpp}; routes overflow to Damage.h (never
// modifies it). 1 world unit = 1 metre; shield points are abstract HP-like
// points (e.g. 0..100), like Health.

#pragma once

#include "scene/World.h"  // PSYNDER_COMPONENT (registers a POD component id)

#include "core/Types.h"

namespace psynder::gameplay {

// Regenerating energy shield carried by an actor. `current` is the live shield
// charge (0..max_shield); `recharge_rate` is points/second once recharge
// resumes; `recharge_delay_s` is how long after the last hit recharge stays
// paused; `time_since_hit_s` counts up from 0 on every hit and gates recharge.
// POD; replicated by the netcode and mutated by the shield systems.
PSYNDER_COMPONENT(Shield) {
    f32 current;           // live shield charge (points), 0..max_shield
    f32 max_shield;        // full charge ceiling (points)
    f32 recharge_rate;     // points per second while recharging
    f32 recharge_delay_s;  // pause (seconds) after a hit before recharge resumes
    f32 time_since_hit_s;  // seconds since the last hit (gates recharge)
};
static_assert(sizeof(Shield) == 20, "Shield layout frozen (5 f32 == 20 bytes)");

// Deal `amount` damage to `victim`, soaking it on the shield before health.
//
// If the victim has a Shield: subtract `amount` from Shield.current first; any
// OVERFLOW (the part of `amount` beyond the remaining shield) is routed to
// gameplay::damage_credited(w, attacker, victim, overflow) so the overflow hits
// Health/Armor and credits the scoreboard exactly like a direct hit. The
// victim's time_since_hit_s is reset to 0 (pausing recharge) whenever a positive
// `amount` lands, even if the shield fully absorbs it.
//
// If the victim has NO Shield: the full `amount` is routed to damage_credited,
// so callers can use this uniformly for shielded and unshielded targets.
//
// `amount <= 0` is a no-op and returns false. Deterministic; no RNG. Returns
// true iff this damage killed the victim (i.e. the overflow was lethal).
bool damage_shielded(scene::World& w, Entity attacker, Entity victim,
                     f32 amount) noexcept;

// Advance every Shield entity by `dt_seconds`: bump time_since_hit_s by dt, and
// once it exceeds recharge_delay_s recharge `current` by recharge_rate*dt, up to
// max_shield (no overshoot). A DEAD entity (Health hp <= 0, or carrying a Dead
// tag) does NOT recharge — its timer still advances but its charge is frozen, so
// a corpse cannot heal its shield. Deterministic: entities are processed in
// ascending entity-id order, no RNG, no per-tick heap alloc.
void tick_shields(scene::World& w, f32 dt_seconds);

// Effective hit points for AI / tests: the victim's remaining shield plus its
// health (Shield.current + Health.hp). A missing Shield contributes 0; a missing
// Health contributes 0. Pure read of the world; no side effects.
f32 effective_hp(scene::World& w, Entity e);

}  // namespace psynder::gameplay
