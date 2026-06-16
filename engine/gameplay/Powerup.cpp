// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/gameplay/Powerup.cpp — see Powerup.h.

#include "gameplay/Powerup.h"

#include "gameplay/GameplayComponents.h"  // Health (regeneration target)

#include <algorithm>
#include <vector>

namespace psynder::gameplay {

void grant_powerup(Powerups& p, PowerupKind k, f32 duration_s) noexcept {
    const u32 i = static_cast<u32>(k);
    const f32 want = (duration_s > 0.0f) ? duration_s : 0.0f;
    // Refresh, never shorten: keep whichever remaining time is longer.
    if (want > p.time_left[i]) p.time_left[i] = want;
}

bool has_powerup(const Powerups& p, PowerupKind k) noexcept {
    return p.time_left[static_cast<u32>(k)] > 0.0f;
}

void tick_powerups(scene::World& w, f32 dt_seconds) {
    // Each entity's timers are independent of every other entity's, so this is
    // order-independent: we can decrement in place during chunk iteration.
    w.for_each_chunk<Powerups>([&](usize n, Powerups* pw) {
        for (usize i = 0; i < n; ++i) {
            for (u32 k = 0; k < kPowerupKindCount; ++k) {
                f32 t = pw[i].time_left[k] - dt_seconds;
                if (t < 0.0f) t = 0.0f;  // clamp at 0 on expiry
                pw[i].time_left[k] = t;
            }
        }
    });
}

f32 damage_multiplier(const Powerups& p) noexcept {
    return has_powerup(p, PowerupKind::QuadDamage) ? 4.0f : 1.0f;
}

f32 incoming_damage_multiplier(const Powerups& p) noexcept {
    return has_powerup(p, PowerupKind::BattleSuit) ? 0.5f : 1.0f;
}

void tick_regeneration(scene::World& w, f32 dt_seconds, f32 hp_per_s,
                       f32 over_max_cap) noexcept {
    // Gather the entities to heal during the read-only walk, then heal in
    // ascending entity-id order so the result is independent of chunk/storage
    // order. The reused statics avoid per-tick heap allocation on the hot path.
    static std::vector<Entity> scratch;
    scratch.clear();
    w.for_each_chunk_with_entities<Powerups, Health>(
        [&](usize n, const Entity* ents, Powerups* pw, Health*) {
            for (usize i = 0; i < n; ++i)
                if (pw[i].time_left[static_cast<u32>(PowerupKind::Regeneration)] >
                    0.0f)
                    scratch.push_back(ents[i]);
        });

    std::sort(scratch.begin(), scratch.end(),
              [](Entity a, Entity b) { return a.raw < b.raw; });

    const f32 heal = hp_per_s * dt_seconds;
    for (const Entity e : scratch) {
        Health* h = w.get<Health>(e);
        if (h == nullptr) continue;
        if (h->hp >= over_max_cap) continue;  // already at/over the overheal cap
        h->hp += heal;
        if (h->hp > over_max_cap) h->hp = over_max_cap;  // clamp at the cap
    }
}

}  // namespace psynder::gameplay
