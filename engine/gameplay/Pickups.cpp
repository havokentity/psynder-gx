// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/gameplay/Pickups.cpp — see Pickups.h.

#include "gameplay/Pickups.h"

#include "scene/GxComponents.h"  // TransformWS

#include <algorithm>
#include <vector>

namespace psynder::gameplay {

namespace {
math::Vec3 translation_of(const scene::TransformWS& t) noexcept {
    return {t.mtw.m[12], t.mtw.m[13], t.mtw.m[14]};
}
}  // namespace

Entity spawn_pickup(scene::World& w, PickupKind kind, f32 amount, math::Vec3 pos,
                    f32 radius_m, f32 respawn_delay_s) {
    const Entity e = w.create();
    scene::TransformWS t{};
    t.mtw = math::translate(pos);
    t.prev_mtw = t.mtw;
    w.add(e, t);
    w.add(e, Pickup{static_cast<u32>(kind), amount, radius_m, respawn_delay_s,
                    0.0f});
    return e;
}

void tick_pickups(scene::World& w, f32 dt_seconds) {
    // Snapshot players (Health entities) once, in ascending id order so the
    // "first overlapping" choice is deterministic.
    struct Player { Entity e; math::Vec3 pos; };
    std::vector<Player> players;
    w.for_each_chunk_with_entities<Health, scene::TransformWS>(
        [&](usize n, const Entity* ents, Health*, scene::TransformWS* xf) {
            for (usize i = 0; i < n; ++i)
                players.push_back({ents[i], translation_of(xf[i])});
        });
    std::sort(players.begin(), players.end(),
              [](const Player& a, const Player& b) { return a.e.raw < b.e.raw; });

    w.for_each_chunk_with_entities<Pickup, scene::TransformWS>(
        [&](usize n, const Entity*, Pickup* pk, scene::TransformWS* xf) {
            for (usize i = 0; i < n; ++i) {
                if (pk[i].cooldown_s > 0.0f) {  // inactive: respawn timer
                    pk[i].cooldown_s -= dt_seconds;
                    if (pk[i].cooldown_s < 0.0f) pk[i].cooldown_s = 0.0f;
                    continue;
                }
                const math::Vec3 pp = translation_of(xf[i]);
                const f32 r2 = pk[i].radius_m * pk[i].radius_m;
                Entity taker{};
                for (const Player& pl : players) {
                    const math::Vec3 d{pp.x - pl.pos.x, pp.y - pl.pos.y,
                                       pp.z - pl.pos.z};
                    if (d.x * d.x + d.y * d.y + d.z * d.z <= r2) {
                        taker = pl.e;  // first by id
                        break;
                    }
                }
                if (!taker.valid()) continue;

                switch (static_cast<PickupKind>(pk[i].kind)) {
                    case PickupKind::Health:
                        if (Health* h = w.get<Health>(taker)) {
                            h->hp += pk[i].amount;
                            if (h->hp > h->max_hp) h->hp = h->max_hp;
                        }
                        break;
                    case PickupKind::Ammo:
                    case PickupKind::Weapon:
                        if (Weapon* wp = w.get<Weapon>(taker)) {
                            if (wp->ammo >= 0)
                                wp->ammo += static_cast<i32>(pk[i].amount);
                        }
                        break;
                }
                pk[i].cooldown_s = pk[i].respawn_delay_s;  // consumed -> inactive
            }
        });
}

}  // namespace psynder::gameplay
