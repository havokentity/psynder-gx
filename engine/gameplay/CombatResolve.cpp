// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/gameplay/CombatResolve.cpp — compose the combat modifiers. See
// CombatResolve.h.

#include "gameplay/CombatResolve.h"

#include "gameplay/Powerup.h"      // Powerups, damage_multiplier
#include "gameplay/Reload.h"       // Magazine, can_fire, consume_round
#include "gameplay/Suppression.h"  // Suppression, spread_multiplier

namespace psynder::gameplay {

ShotResult begin_shot(scene::World& w, Entity shooter,
                      f32 base_spread_tan) noexcept {
    ShotResult r;
    r.fired = true;
    r.spread_tan = base_spread_tan;
    r.damage_scale = 1.0f;

    // Ammo gate: a shooter with a Magazine must be able to fire; spend a round.
    // A shooter with no Magazine fires freely (e.g. infinite-ammo bots).
    if (Magazine* m = w.get<Magazine>(shooter)) {
        if (!can_fire(*m)) {
            r.fired = false;
            return r;  // blocked — no round spent, no modifiers needed
        }
        consume_round(*m);
    }

    // Suppression widens the cone (less accurate while being shot at).
    if (const Suppression* s = w.get<Suppression>(shooter)) {
        r.spread_tan = base_spread_tan * spread_multiplier(*s);
    }

    // Powerups (QuadDamage) scale the outgoing damage.
    if (const Powerups* p = w.get<Powerups>(shooter)) {
        r.damage_scale = damage_multiplier(*p);
    }

    return r;
}

f32 resolve_damage(f32 base_damage, f32 distance_m, Hitbox hb,
                   const FalloffProfile& falloff,
                   const ShotResult& shot) noexcept {
    return resolve_ranged_damage(base_damage, distance_m, hb, falloff) *
           shot.damage_scale;
}

}  // namespace psynder::gameplay
