// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/gameplay/StatusEffect.cpp — see StatusEffect.h.

#include "gameplay/StatusEffect.h"

#include "gameplay/Damage.h"             // damage_credited / apply_damage
#include "gameplay/GameplayComponents.h" // Health, Dead

#include <algorithm>
#include <vector>

namespace psynder::gameplay {

void apply_status(StatusEffects& s, StatusKind k, f32 duration_s, f32 dps,
                  Entity source) noexcept {
    const u32 i = static_cast<u32>(k);
    const f32 want = (duration_s > 0.0f) ? duration_s : 0.0f;

    // Refresh, never shorten: keep whichever remaining time is longer (the
    // refresh-not-stack rule — re-applying extends, it does not add a second
    // instance).
    if (want > s.time_left[i]) s.time_left[i] = want;

    // Take the higher damage rate so a stronger application wins and a weaker
    // one cannot weaken an active effect.
    if (dps > s.dps[i]) s.dps[i] = dps;

    // The most recent application owns the kill credit for this kind.
    s.source[i] = source.raw;
}

bool has_status(const StatusEffects& s, StatusKind k) noexcept {
    return s.time_left[static_cast<u32>(k)] > 0.0f;
}

namespace {
// One pending DoT hit gathered during the read-only walk and resolved after.
struct PendingTick {
    Entity victim;
    Entity source;
    f32    amount;
};
}  // namespace

void tick_status(scene::World& w, f32 dt_seconds) {
    if (dt_seconds <= 0.0f) return;

    // Gather the damage to deal during a read-only walk: decrement the timers in
    // place (order-independent), accumulate each active kind's dps*dt hit, and
    // clear expired kinds. Structural changes (the Dead tag a lethal hit adds)
    // happen AFTER the walk, so iterating chunks here is safe. The reused static
    // avoids per-tick heap allocation on the hot path.
    static std::vector<PendingTick> pending;
    pending.clear();

    w.for_each_chunk_with_entities<StatusEffects, Health>(
        [&](usize n, const Entity* ents, StatusEffects* se, Health*) {
            for (usize idx = 0; idx < n; ++idx) {
                StatusEffects& s = se[idx];
                // No DoT on a corpse: clear every effect on a dead entity.
                if (w.get<Dead>(ents[idx]) != nullptr) {
                    for (u32 k = 0; k < kStatusKindCount; ++k) {
                        s.time_left[k] = 0.0f;
                        s.dps[k] = 0.0f;
                        s.source[k] = 0u;
                    }
                    continue;
                }
                for (u32 k = 0; k < kStatusKindCount; ++k) {
                    if (s.time_left[k] <= 0.0f) continue;
                    // Damage is bled continuously as dps*dt this tick (the total
                    // over the duration is ~= dps*duration).
                    const f32 amount = s.dps[k] * dt_seconds;
                    if (amount > 0.0f) {
                        pending.push_back(
                            PendingTick{ents[idx], Entity{s.source[k]}, amount});
                    }
                    f32 t = s.time_left[k] - dt_seconds;
                    if (t <= 0.0f) {
                        // Expired: clear the slot (rate + source reset too).
                        s.time_left[k] = 0.0f;
                        s.dps[k] = 0.0f;
                        s.source[k] = 0u;
                    } else {
                        s.time_left[k] = t;
                    }
                }
            }
        });

    // Resolve in ascending victim-id order (kinds already gathered in enum
    // order per victim) so the outcome is independent of chunk/storage order.
    // A stable sort preserves the per-victim enum order of the hits.
    std::stable_sort(pending.begin(), pending.end(),
                     [](const PendingTick& a, const PendingTick& b) {
                         return a.victim.raw < b.victim.raw;
                     });

    for (const PendingTick& p : pending) {
        // Credit the attacker who applied the effect. If the source is invalid
        // or the victim itself, fall back to an uncredited apply_damage.
        if (p.source.valid() && p.source.raw != p.victim.raw) {
            damage_credited(w, p.source, p.victim, p.amount);
        } else {
            apply_damage(w, p.victim, p.amount);
        }
    }
}

}  // namespace psynder::gameplay
