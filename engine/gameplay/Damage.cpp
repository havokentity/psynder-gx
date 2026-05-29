// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/gameplay/Damage.cpp — see Damage.h.

#include "gameplay/Damage.h"

#include "gameplay/GameplayComponents.h"
#include "scene/GxComponents.h"  // TransformWS (respawn move)

#include <algorithm>

namespace psynder::gameplay {

bool apply_damage(scene::World& w, Entity e, f32 amount) noexcept {
    if (amount <= 0.0f) return false;
    Health* h = w.get<Health>(e);
    if (h == nullptr || h->hp <= 0.0f) return false;  // not damageable / already dead

    f32 to_health = amount;
    if (Armor* a = w.get<Armor>(e); a != nullptr && a->points > 0.0f) {
        const f32 want = amount * kArmorAbsorbFraction;
        const f32 absorbed = (want < a->points) ? want : a->points;  // cap at points
        a->points -= absorbed;
        to_health = amount - absorbed;
    }

    h->hp -= to_health;
    if (h->hp <= 0.0f) {
        h->hp = 0.0f;
        f32 delay = kDefaultRespawnDelay;
        if (const Respawnable* r = w.get<Respawnable>(e)) delay = r->delay_s;
        if (w.get<Dead>(e) == nullptr) w.add(e, Dead{delay});
        return true;
    }
    return false;
}

void update_respawns(scene::World& w, f32 dt_seconds, std::vector<Entity>& scratch) {
    // Tick timers in place (order-independent), gathering the entities that have
    // come due. Structural changes (respawn) happen AFTER the walk.
    scratch.clear();
    w.for_each_chunk_with_entities<Dead>(
        [&](usize n, const Entity* ents, Dead* d) {
            for (usize i = 0; i < n; ++i) {
                d[i].respawn_in_s -= dt_seconds;
                if (d[i].respawn_in_s <= 0.0f) scratch.push_back(ents[i]);
            }
        });

    // Respawn in ascending entity-id order so the result is deterministic
    // regardless of chunk/storage order.
    std::sort(scratch.begin(), scratch.end(),
              [](Entity a, Entity b) { return a.raw < b.raw; });
    for (const Entity e : scratch) {
        if (Health* h = w.get<Health>(e)) h->hp = h->max_hp;
        if (const Respawnable* r = w.get<Respawnable>(e)) {
            if (scene::TransformWS* t = w.get<scene::TransformWS>(e)) {
                t->prev_mtw = t->mtw;
                t->mtw.m[12] = r->spawn_pos.x;
                t->mtw.m[13] = r->spawn_pos.y;
                t->mtw.m[14] = r->spawn_pos.z;
            }
        }
        w.remove<Dead>(e);
    }
}

}  // namespace psynder::gameplay
