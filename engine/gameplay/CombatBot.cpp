// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/gameplay/CombatBot.cpp — see CombatBot.h.

#include "gameplay/CombatBot.h"

#include "gameplay/GameplayComponents.h"  // Health, Weapon
#include "gameplay/Weapons.h"             // fire_hitscan
#include "physics/agents/AgentComponents.h"  // scene::AgentTarget
#include "scene/GxComponents.h"           // TransformWS

#include <algorithm>
#include <vector>

namespace psynder::gameplay {

namespace {
math::Vec3 trans(const scene::TransformWS& t) noexcept {
    return {t.mtw.m[12], t.mtw.m[13], t.mtw.m[14]};
}
}  // namespace

void tick_combat_bots(scene::World& w, const ai::FlowField& field, f32 /*dt*/) {
    // Gather live combatants (Health alive + Team + pos) for enemy search.
    struct Combatant { Entity e; u32 team; math::Vec3 pos; };
    std::vector<Combatant> combatants;
    w.for_each_chunk_with_entities<Health, Team, scene::TransformWS>(
        [&](usize n, const Entity* ents, Health* h, Team* tm,
            scene::TransformWS* xf) {
            for (usize i = 0; i < n; ++i)
                if (h[i].hp > 0.0f)
                    combatants.push_back({ents[i], tm[i].team, trans(xf[i])});
        });

    // Bots, processed in ascending id order for deterministic firing.
    std::vector<Entity> bots;
    w.for_each_chunk_with_entities<Bot>(
        [&](usize n, const Entity* ents, Bot*) {
            for (usize i = 0; i < n; ++i) bots.push_back(ents[i]);
        });
    std::sort(bots.begin(), bots.end(),
              [](Entity a, Entity b) { return a.raw < b.raw; });

    for (const Entity be : bots) {
        const Bot* bot = w.get<Bot>(be);
        const Health* bh = w.get<Health>(be);
        const Team* bt = w.get<Team>(be);
        scene::TransformWS* bx = w.get<scene::TransformWS>(be);
        if (!bot || !bt || !bx || !bh || bh->hp <= 0.0f) continue;
        const math::Vec3 pos = trans(*bx);

        // PATH: steer along the flow field toward the goal.
        if (scene::AgentTarget* tgt = w.get<scene::AgentTarget>(be)) {
            const math::Vec3 flow = field.sample(pos);
            if (flow.x != 0.0f || flow.z != 0.0f) {
                tgt->goal = {pos.x + flow.x * bot->lookahead_m, pos.y,
                             pos.z + flow.z * bot->lookahead_m};
            } else {
                tgt->goal = pos;  // at goal / no path -> hold
            }
        }

        // SHOOT: nearest live enemy within fire_range (ties -> lower id).
        Entity foe{};
        math::Vec3 foe_pos{};
        f32 best_d2 = bot->fire_range_m * bot->fire_range_m;
        bool found = false;
        for (const Combatant& c : combatants) {
            if (c.team == bt->team || c.e.raw == be.raw) continue;
            const math::Vec3 d{c.pos.x - pos.x, c.pos.y - pos.y, c.pos.z - pos.z};
            const f32 d2 = d.x * d.x + d.y * d.y + d.z * d.z;
            if (d2 > best_d2) continue;
            if (!found || d2 < best_d2 || (d2 == best_d2 && c.e.raw < foe.raw)) {
                found = true;
                best_d2 = d2;
                foe = c.e;
                foe_pos = c.pos;
            }
        }
        if (found) {
            const math::Vec3 dir{foe_pos.x - pos.x, foe_pos.y - pos.y,
                                 foe_pos.z - pos.z};
            fire_hitscan(w, be, pos, dir);  // gates on cooldown/ammo; credits kill
        }
    }
}

}  // namespace psynder::gameplay
