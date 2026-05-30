// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/gameplay/CombatBot.cpp — see CombatBot.h.

#include "gameplay/CombatBot.h"

#include "gameplay/GameplayComponents.h"  // Health, Weapon, Team
#include "gameplay/Weapons.h"             // fire_hitscan
#include "physics/agents/AgentComponents.h"  // scene::AgentTarget
#include "scene/GxComponents.h"           // TransformWS
#include "scene/Spatial.h"                // UniformGrid broadphase

#include <algorithm>
#include <vector>

namespace psynder::gameplay {

namespace {
math::Vec3 trans(const scene::TransformWS& t) noexcept {
    return {t.mtw.m[12], t.mtw.m[13], t.mtw.m[14]};
}
}  // namespace

void tick_combat_bots(scene::World& w, const ai::FlowField& field, f32 /*dt*/) {
    // Bots first: collect (sorted for deterministic firing order) + the max fire
    // range, which sizes the broadphase grid cell.
    std::vector<Entity> bots;
    w.for_each_chunk_with_entities<Bot>([&](usize n, const Entity* ents, Bot*) {
        for (usize i = 0; i < n; ++i) bots.push_back(ents[i]);
    });
    std::sort(bots.begin(), bots.end(),
              [](Entity a, Entity b) { return a.raw < b.raw; });

    f32 max_range = 2.0f;
    for (const Entity be : bots) {
        if (const Bot* b = w.get<Bot>(be))
            max_range = std::max(max_range, b->fire_range_m);
    }

    // Live combatants (alive Health + Team + pos). A parallel AABB array (point
    // proxies) feeds a UniformGrid so the per-bot enemy search is a local sphere
    // query (O(bots·neighbours)) instead of O(bots·combatants) — scale-ready for
    // thousands of agents (§1b broadphase mandate). Results are identical to the
    // former linear scan: the SAME in-range + nearest-by-id-tie predicate runs on
    // the (superset) candidates the grid returns, so determinism is preserved.
    struct Combatant { Entity e; u32 team; math::Vec3 pos; };
    std::vector<Combatant> combatants;
    std::vector<math::Aabb> proxies;
    w.for_each_chunk_with_entities<Health, Team, scene::TransformWS>(
        [&](usize n, const Entity* ents, Health* h, Team* tm,
            scene::TransformWS* xf) {
            for (usize i = 0; i < n; ++i) {
                if (h[i].hp <= 0.0f) continue;
                const math::Vec3 p = trans(xf[i]);
                combatants.push_back({ents[i], tm[i].team, p});
                proxies.push_back(math::Aabb{p, p});  // point proxy
            }
        });

    scene::UniformGrid grid;
    if (!proxies.empty()) grid.build(proxies, max_range);
    std::vector<u32> candidates;  // reused across bots (cleared per query)

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

        // SHOOT: nearest live enemy within fire_range (ties -> lower id). The grid
        // returns a superset of in-range candidates; the exact predicate below
        // matches the former linear scan exactly.
        Entity foe{};
        math::Vec3 foe_pos{};
        f32 best_d2 = bot->fire_range_m * bot->fire_range_m;
        bool found = false;
        candidates.clear();
        if (!proxies.empty())
            grid.query_sphere(pos, bot->fire_range_m + 0.5f, candidates);
        for (const u32 idx : candidates) {
            const Combatant& c = combatants[idx];
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
            // Team-aware: the ray shoots through teammates (friendly fire off).
            fire_hitscan(w, be, pos, dir, static_cast<i64>(bt->team));
        }
    }
}

}  // namespace psynder::gameplay
