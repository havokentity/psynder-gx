// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/gameplay/Heal.cpp — see Heal.h.

#include "gameplay/Heal.h"

#include "gameplay/GameplayComponents.h"  // Health (heal target)

#include <algorithm>

namespace psynder::gameplay {

f32 apply_heal(scene::World& w, Entity e, f32 amount, f32 over_max_cap) noexcept {
    if (amount <= 0.0f) return 0.0f;
    Health* h = w.get<Health>(e);
    if (h == nullptr || h->hp <= 0.0f) return 0.0f;  // no health / dead: not healed
    if (h->hp >= over_max_cap) return 0.0f;          // already at/over the ceiling

    const f32 before = h->hp;
    f32 after = before + amount;
    if (after > over_max_cap) after = over_max_cap;  // clamp at the cap
    h->hp = after;
    return after - before;  // actual amount restored
}

void grant_hot(scene::World& w, Entity e, f32 rate_per_s, f32 duration_s,
               f32 over_max_cap) noexcept {
    const f32 rate = (rate_per_s > 0.0f) ? rate_per_s : 0.0f;
    const f32 dur = (duration_s > 0.0f) ? duration_s : 0.0f;
    const f32 cap = (over_max_cap > 0.0f) ? over_max_cap : 0.0f;

    if (HealOverTime* hot = w.get<HealOverTime>(e); hot != nullptr) {
        // Refresh, never downgrade: keep the stronger field of each independently.
        if (rate > hot->rate_per_s) hot->rate_per_s = rate;
        if (dur > hot->remaining_s) hot->remaining_s = dur;
        if (cap > hot->over_max_cap) hot->over_max_cap = cap;
        return;
    }
    if (dur <= 0.0f) return;  // nothing to attach (no existing buff, no duration)
    w.add(e, HealOverTime{rate, dur, cap});
}

void tick_heals(scene::World& w, f32 dt_s, std::vector<Entity>& scratch) noexcept {
    // Tick timers in place (order-independent), gathering every buffed entity.
    // Healing + the structural removal of expired buffs happen AFTER the walk so
    // we never mutate storage while iterating it.
    scratch.clear();
    w.for_each_chunk_with_entities<HealOverTime>(
        [&](usize n, const Entity* ents, HealOverTime* hot) {
            for (usize i = 0; i < n; ++i) {
                hot[i].remaining_s -= dt_s;
                scratch.push_back(ents[i]);
            }
        });

    // Apply in ascending entity-id order so the result is independent of
    // chunk/storage order.
    std::sort(scratch.begin(), scratch.end(),
              [](Entity a, Entity b) { return a.raw < b.raw; });

    for (const Entity e : scratch) {
        HealOverTime* hot = w.get<HealOverTime>(e);
        if (hot == nullptr) continue;  // defensive: removed mid-pass
        // Heal this step through the same cap logic as apply_heal. A non-positive
        // dt yields a non-positive amount, which apply_heal treats as a no-op.
        apply_heal(w, e, hot->rate_per_s * dt_s, hot->over_max_cap);
        if (hot->remaining_s <= 0.0f) w.remove<HealOverTime>(e);  // expired
    }
}

}  // namespace psynder::gameplay
