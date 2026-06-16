// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/gameplay/Shield.cpp — see Shield.h.

#include "gameplay/Shield.h"

#include "gameplay/Damage.h"             // damage_credited (overflow path)
#include "gameplay/GameplayComponents.h" // Health, Dead

#include <algorithm>  // std::sort, std::min
#include <vector>

namespace psynder::gameplay {

bool damage_shielded(scene::World& w, Entity attacker, Entity victim,
                     f32 amount) noexcept {
    if (amount <= 0.0f) return false;

    Shield* s = w.get<Shield>(victim);
    if (s == nullptr) {
        // No shield: behave exactly like a credited direct hit.
        return damage_credited(w, attacker, victim, amount);
    }

    // Soak on the shield first; a positive hit always pauses recharge, even if
    // the shield fully absorbs it.
    s->time_since_hit_s = 0.0f;

    const f32 soaked = (amount < s->current) ? amount : s->current;
    s->current -= soaked;
    const f32 overflow = amount - soaked;  // >= 0; part of the hit beyond the shield

    if (overflow > 0.0f) {
        return damage_credited(w, attacker, victim, overflow);
    }
    return false;  // shield absorbed the whole hit; nothing reached health
}

void tick_shields(scene::World& w, f32 dt_seconds) {
    // Gather the Shield entities, then process in ascending entity-id order so
    // the result is independent of chunk/storage order. The scratch buffer is a
    // function-local reused vector; advancing the timer in place is
    // order-independent, but recharge reads Health (a non-Shield column) per
    // entity, so we materialise the ids once and sort for determinism.
    static std::vector<Entity> scratch;  // reused across ticks; single-threaded sim
    scratch.clear();
    w.for_each_chunk_with_entities<Shield>(
        [&](usize n, const Entity* ents, Shield* /*sh*/) {
            for (usize i = 0; i < n; ++i) scratch.push_back(ents[i]);
        });
    std::sort(scratch.begin(), scratch.end(),
              [](Entity a, Entity b) { return a.raw < b.raw; });

    for (const Entity e : scratch) {
        Shield* s = w.get<Shield>(e);
        if (s == nullptr) continue;  // defensive: removed mid-walk

        s->time_since_hit_s += dt_seconds;

        // A dead actor does not recharge its shield (timer still advances).
        bool dead = (w.get<Dead>(e) != nullptr);
        if (!dead) {
            if (const Health* h = w.get<Health>(e)) dead = (h->hp <= 0.0f);
        }
        if (dead) continue;

        if (s->time_since_hit_s > s->recharge_delay_s &&
            s->current < s->max_shield) {
            s->current += s->recharge_rate * dt_seconds;
            if (s->current > s->max_shield) s->current = s->max_shield;  // no overshoot
        }
    }
}

f32 effective_hp(scene::World& w, Entity e) {
    f32 total = 0.0f;
    if (const Shield* s = w.get<Shield>(e)) total += s->current;
    if (const Health* h = w.get<Health>(e)) total += h->hp;
    return total;
}

}  // namespace psynder::gameplay
