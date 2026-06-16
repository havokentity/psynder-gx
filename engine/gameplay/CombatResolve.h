// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/gameplay/CombatResolve.h — the shot-resolution INTEGRATION that
// composes the combat modifier systems into one place, so a fire path
// (fire_hitscan and friends) can ask a single question: "given THIS shooter,
// does the shot fire, how wide is its cone, and how much does its damage scale?"
//
// It wires together the per-shooter combat state built across the session:
//   * Magazine (Reload.h)     — the ammo gate: a shooter with a magazine must
//                               have a round chambered and not be mid-reload;
//                               firing spends a round.
//   * Suppression (Suppression.h) — incoming fire widens the shooter's spread
//                               cone (a suppressed shooter is less accurate).
//   * Powerups (Powerup.h)    — QuadDamage scales the outgoing damage.
//   * RangedDamage / Ballistics — distance falloff + hitbox shaping of damage.
//
// Pure / deterministic / lockstep-safe: reads components, no RNG, no allocation.
// The components are optional — a shooter without a Magazine fires freely, one
// without Suppression has its base spread, one without Powerups deals 1x damage —
// so existing callers that have none of them are unchanged.

#pragma once

#include "gameplay/Ballistics.h"     // Hitbox
#include "gameplay/RangedDamage.h"   // FalloffProfile, resolve_ranged_damage

#include "scene/World.h"

#include "core/Types.h"

namespace psynder::gameplay {

// The resolved per-shot modifiers for a given shooter this trigger pull.
struct ShotResult {
    bool fired = true;        // false when an ammo gate (Magazine) blocked the shot
    f32  spread_tan = 0.0f;   // effective cone half-angle as tan (base * suppression)
    f32  damage_scale = 1.0f; // outgoing damage multiplier (e.g. 4x QuadDamage)
};

// Begin a shot for `shooter`: apply the ammo gate (Magazine) and read the spread
// + damage modifiers. If the shooter has a Magazine and cannot fire (empty or
// mid-reload) the result is {fired=false} and NO round is spent; otherwise one
// round is consumed. `base_spread_tan` is the weapon's intrinsic cone (0 = pin-
// point); Suppression multiplies it, Powerups (Quad) set the damage scale. A
// shooter missing any of these components just gets the neutral default for it.
ShotResult begin_shot(scene::World& w, Entity shooter, f32 base_spread_tan) noexcept;

// Final outgoing damage for a hit: the distance-/hitbox-shaped ranged damage
// (RangedDamage) scaled by the shot's damage multiplier (Powerups). Pure algebra.
f32 resolve_damage(f32 base_damage, f32 distance_m, Hitbox hb,
                   const FalloffProfile& falloff, const ShotResult& shot) noexcept;

}  // namespace psynder::gameplay
