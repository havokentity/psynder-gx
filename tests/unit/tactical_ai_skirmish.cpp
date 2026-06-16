// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/tactical_ai_skirmish.cpp — the tactical-AI stack integrated as a
// headless game, composing the per-bot AI primitives built this session into
// one deterministic loop:
//   * ai::TargetSelect   — pick the best enemy by a composite score;
//   * ai::ThreatMemory   — remember a last-seen enemy after losing line of sight;
//   * ai::CoverScore     — when hurt, choose the safest cover position;
//   * ai::CoverPoints    — the wall-adjacent cover cells to choose among;
//   * ai::line_of_sight  — gate target acquisition through the wall;
//   * ai::NavAgent       — A* navigation to the chosen goal;
//   * gameplay::TacticalBot — the Patrol/Engage/Retreat fight-or-flight FSM;
//   * gameplay::WeaponLoadout + fire_hitscan + Damage — the actual combat.
// Two bot teams fight across a walled arena with a chokepoint; bots acquire
// targets, remember them, fire, and fall back to scored cover when hurt — all
// bit-reproducibly, which proves every composed subsystem is lockstep-safe.

#include "ai/CoverPoints.h"
#include "ai/CoverScore.h"
#include "ai/GridAStar.h"
#include "ai/NavAgent.h"
#include "ai/PathFollow.h"     // GridLayout, cell_to_world
#include "ai/PathSimplify.h"   // line_of_sight
#include "ai/TargetSelect.h"
#include "ai/ThreatMemory.h"

#include "gameplay/Damage.h"
#include "gameplay/GameplayComponents.h"
#include "gameplay/RangedDamage.h"
#include "gameplay/TacticalBot.h"
#include "gameplay/WeaponLoadout.h"
#include "gameplay/Weapons.h"

#include "scene/GxComponents.h"  // TransformWS
#include "scene/World.h"

#include "math/Math.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

using namespace psynder;
using namespace psynder::gameplay;
using psynder::scene::TransformWS;
using psynder::scene::World;

namespace {

constexpr u32 kGrid = 20;
constexpr u32 kPerTeam = 4;
constexpr f32 kDt = 1.0f / 60.0f;
constexpr f32 kSpeed = 5.0f;
constexpr f32 kFireRange = 16.0f;
constexpr f32 kRetreatFrac = 0.5f;
constexpr u32 kWallZ = 10;
constexpr u32 kGapLo = 8, kGapHi = 11;

math::Vec3 epos(World& w, Entity e) {
    const TransformWS* xf = w.get<TransformWS>(e);
    return {xf->mtw.m[12], xf->mtw.m[13], xf->mtw.m[14]};
}
math::Vec3 unit_xz(math::Vec3 v) {
    const f32 l = std::sqrt(v.x * v.x + v.z * v.z);
    if (l <= 1.0e-6f) return {0.0f, 0.0f, 0.0f};
    return {v.x / l, 0.0f, v.z / l};
}

void build_grid(ai::GridAStar& g) {
    g.resize(kGrid, kGrid);
    for (u32 x = 0; x < kGrid; ++x) {
        if (x < kGapLo || x > kGapHi) g.set_blocked(x, kWallZ, true);
    }
}

Entity spawn(World& w, u32 team, math::Vec3 pos) {
    const Entity e = w.create();
    TransformWS t{};
    t.mtw = math::translate(pos);
    t.prev_mtw = t.mtw;
    w.add(e, t);
    w.add(e, Team{team});
    w.add(e, Health{100.0f, 100.0f});
    Weapon wp{};
    equip_weapon(wp, WeaponClass::MachineGun);
    wp.ammo = -1;
    wp.hit_radius = 0.7f;
    w.add(e, wp);
    w.add(e, Score{0, 0});
    w.add(e, Respawnable{pos, 1.0f});
    w.add(e, TacticalBot{static_cast<u32>(TacticalState::Patrol), kRetreatFrac});
    return e;
}

struct Result {
    std::vector<f32> sig;
    u32 frags = 0, deaths = 0;
    bool any_retreat = false;
    bool any_sighting = false;
};

Result run(u32 ticks) {
    World w;
    ai::GridAStar grid;
    build_grid(grid);
    const ai::GridLayout layout{1.0f, 0.0f, 0.0f, kGrid};
    const FalloffProfile falloff = weapon_spec(WeaponClass::MachineGun).falloff;

    // Cover cell world centres (wall-adjacent), shared retreat candidates.
    std::vector<u32> cover_cells;
    ai::find_cover_cells(grid, cover_cells);
    std::vector<math::Vec3> cover_world;
    cover_world.reserve(cover_cells.size());
    for (const u32 c : cover_cells) cover_world.push_back(ai::cell_to_world(c, layout));

    std::vector<Entity> bots;
    std::vector<ai::NavAgent> navs;
    std::vector<ai::ThreatMemory> mem;
    const u32 cols[kPerTeam] = {7, 9, 11, 13};
    for (u32 i = 0; i < kPerTeam; ++i) {
        bots.push_back(spawn(w, 0, {static_cast<f32>(cols[i]) + 0.5f, 0.0f, 4.5f}));
        bots.push_back(spawn(w, 1, {static_cast<f32>(cols[i]) + 0.5f, 0.0f, 15.5f}));
        ai::NavAgent n0{}; n0.layout = layout; n0.follower.arrival_radius_m = 0.6f;
        ai::NavAgent n1{}; n1.layout = layout; n1.follower.arrival_radius_m = 0.6f;
        navs.push_back(n0); navs.push_back(n1);
        mem.emplace_back(8u); mem.emplace_back(8u);
    }
    const u32 obj_x[2] = {9, 9};
    const u32 obj_z[2] = {16, 4};

    std::vector<TacticalState> action(bots.size(), TacticalState::Patrol);
    std::vector<math::Vec3> enemy_pos_scratch;
    std::vector<Entity> dscratch;
    Result r;

    for (u32 t = 0; t < ticks; ++t) {
        // Phase 1: perceive, decide, fire.
        for (usize i = 0; i < bots.size(); ++i) {
            const Entity e = bots[i];
            const Health* h = w.get<Health>(e);
            if (h == nullptr || h->hp <= 0.0f) continue;
            const u32 team = w.get<Team>(e)->team;
            const math::Vec3 pos = epos(w, e);
            const usize bcell = ai::world_to_cell(pos, layout, kGrid);
            const u32 bx = static_cast<u32>(bcell % kGrid), bz = static_cast<u32>(bcell / kGrid);

            // Build enemy candidates (all living other-team bots).
            std::vector<ai::TargetCandidate> cands;
            for (usize j = 0; j < bots.size(); ++j) {
                const Health* oh = w.get<Health>(bots[j]);
                if (oh == nullptr || oh->hp <= 0.0f) continue;
                if (w.get<Team>(bots[j])->team == team) continue;
                const math::Vec3 op = epos(w, bots[j]);
                const usize ec = ai::world_to_cell(op, layout, kGrid);
                const u32 ex = static_cast<u32>(ec % kGrid), ez = static_cast<u32>(ec / kGrid);
                const f32 dx = op.x - pos.x, dz = op.z - pos.z;
                const bool vis = (dx * dx + dz * dz) <= kFireRange * kFireRange &&
                                 ai::line_of_sight(grid, bx, bz, ex, ez);
                cands.push_back({100u + static_cast<u32>(j), op, w.get<Team>(bots[j])->team,
                                 oh->hp, vis});
            }

            u32 target_id = 0;
            bool have_target = ai::select_target(pos, team, cands, ai::kDefaultTargetWeights,
                                                 target_id);
            bool enemy_visible = false;
            math::Vec3 target_pos{};
            if (have_target) {
                for (const auto& c : cands) {
                    if (c.id == target_id) {
                        enemy_visible = c.visible;
                        target_pos = c.pos;
                        break;
                    }
                }
                if (enemy_visible) {
                    mem[i].record(target_id, target_pos);
                    r.any_sighting = true;
                }
            }
            mem[i].tick(kDt, /*forget_after_s=*/3.0f);

            const TacticalState st =
                decide_tactical_state(h->hp, h->max_hp, enemy_visible, kRetreatFrac);
            action[i] = st;
            w.get<TacticalBot>(e)->state = static_cast<u32>(st);
            if (st == TacticalState::Retreat) r.any_retreat = true;

            if (st == TacticalState::Engage && enemy_visible) {
                const math::Vec3 dir = unit_xz({target_pos.x - pos.x, 0.0f, target_pos.z - pos.z});
                if (dir.x != 0.0f || dir.z != 0.0f) {
                    fire_hitscan(w, e, pos, dir, static_cast<i64>(team), 0.0f, 0, &falloff);
                }
            }
        }

        // Phase 2: navigate + move.
        for (usize i = 0; i < bots.size(); ++i) {
            const Entity e = bots[i];
            const Health* h = w.get<Health>(e);
            if (h == nullptr || h->hp <= 0.0f) continue;
            if (action[i] == TacticalState::Engage) continue;  // hold + shoot
            const u32 team = w.get<Team>(e)->team;
            const math::Vec3 pos = epos(w, e);

            u32 gx = obj_x[team], gz = obj_z[team];
            if (action[i] == TacticalState::Retreat && !cover_world.empty()) {
                // Threats = current enemy positions; pick the safest cover near us.
                enemy_pos_scratch.clear();
                for (usize j = 0; j < bots.size(); ++j) {
                    const Health* oh = w.get<Health>(bots[j]);
                    if (oh && oh->hp > 0.0f && w.get<Team>(bots[j])->team != team)
                        enemy_pos_scratch.push_back(epos(w, bots[j]));
                }
                usize cidx = 0;
                if (ai::best_cover(cover_world, enemy_pos_scratch, pos,
                                   ai::kDefaultCoverWeights, cidx)) {
                    const usize cc = ai::world_to_cell(cover_world[cidx], layout, kGrid);
                    gx = static_cast<u32>(cc % kGrid);
                    gz = static_cast<u32>(cc / kGrid);
                }
            } else if (action[i] == TacticalState::Patrol) {
                // No visible enemy: search a remembered last-seen position if any.
                ai::ThreatEntry mem_target{};
                if (mem[i].freshest(mem_target)) {
                    const usize mc = ai::world_to_cell(mem_target.last_pos, layout, kGrid);
                    gx = static_cast<u32>(mc % kGrid);
                    gz = static_cast<u32>(mc / kGrid);
                }
            }
            if (navs[i].goal_x != gx || navs[i].goal_z != gz || !navs[i].has_goal) {
                ai::set_goal(navs[i], gx, gz);
            }
            const math::Vec3 steer = ai::update(navs[i], grid, pos);
            const math::Vec3 np{pos.x + steer.x * kSpeed * kDt, 0.0f, pos.z + steer.z * kSpeed * kDt};
            TransformWS* xf = w.get<TransformWS>(e);
            xf->prev_mtw = xf->mtw;
            xf->mtw = math::translate(np);
        }

        tick_weapons(w, kDt);
        update_respawns(w, kDt, dscratch);
    }

    for (Entity e : bots) {
        const Health* h = w.get<Health>(e);
        const Score* s = w.get<Score>(e);
        const math::Vec3 p = epos(w, e);
        r.sig.push_back(h->hp);
        r.sig.push_back(static_cast<f32>(s->frags));
        r.sig.push_back(p.x);
        r.sig.push_back(p.z);
        r.frags += s->frags;
        r.deaths += s->deaths;
    }
    return r;
}

}  // namespace

TEST_CASE("tactical AI: target select, threat memory, cover scoring, and nav compose",
          "[tactical][ai][gameplay]") {
    const Result a = run(600);
    // The composed loop produced a fight, and the bots acquired targets.
    REQUIRE(a.frags + a.deaths > 0u);
    REQUIRE(a.any_sighting);   // TargetSelect + line_of_sight + ThreatMemory fired
    REQUIRE(a.any_retreat);    // TacticalBot Retreat + CoverScore engaged
}

TEST_CASE("tactical AI: the whole composed loop is deterministic",
          "[tactical][determinism]") {
    const Result a = run(600);
    const Result b = run(600);
    REQUIRE(a.sig == b.sig);
    REQUIRE(a.frags == b.frags);
    REQUIRE(a.deaths == b.deaths);
    REQUIRE(a.any_retreat == b.any_retreat);
}
