// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/tactical_skirmish.cpp — the tactical-bot stack integrated as a
// headless game loop, proving the iter-26..30 building blocks COMPOSE. Two teams
// of bots fight across a walled arena with a single chokepoint:
//   * ai::NavAgent (A* + string-pull) routes each bot toward its objective /
//     cover through the gap in the wall;
//   * ai::line_of_sight gates target acquisition (no shooting through the wall);
//   * gameplay::WeaponLoadout equips a real archetype + its RangedDamage falloff
//     scales fire_hitscan by range;
//   * gameplay::TacticalBot's FSM picks Patrol / Engage / Retreat from health +
//     visibility, and a Retreating bot falls back to ai::CoverPoints' nearest
//     wall-adjacent cover cell;
//   * the existing Weapons / Damage / respawn systems resolve the combat.
// The whole loop is deterministic — bit-reproducible across two runs — which is
// the real assertion: every composed subsystem is lockstep-safe.

#include "ai/CoverPoints.h"
#include "ai/GridAStar.h"
#include "ai/NavAgent.h"
#include "ai/PathFollow.h"     // GridLayout, cell_to_world
#include "ai/PathSimplify.h"   // line_of_sight

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

constexpr u32 kGrid = 24;
constexpr u32 kPerTeam = 5;
constexpr f32 kDt = 1.0f / 60.0f;
constexpr f32 kSpeed = 5.0f;       // bot move speed (m/s)
constexpr f32 kFireRange = 18.0f;  // engage within this distance (m)
constexpr f32 kRetreatFrac = 0.5f;
constexpr u32 kWallZ = 12;         // the dividing wall row
constexpr u32 kGapLo = 10, kGapHi = 13;  // free gap cells [10..13] on the wall row

math::Vec3 entity_pos(World& w, Entity e) {
    const TransformWS* xf = w.get<TransformWS>(e);
    return {xf->mtw.m[12], xf->mtw.m[13], xf->mtw.m[14]};
}

math::Vec3 unit_xz(math::Vec3 v) {
    const f32 len = std::sqrt(v.x * v.x + v.z * v.z);
    if (len <= 1.0e-6f) return {0.0f, 0.0f, 0.0f};
    return {v.x / len, 0.0f, v.z / len};
}

// Build the shared nav/cover/LoS grid: a wall across z == kWallZ with a gap.
void build_grid(ai::GridAStar& grid) {
    grid.resize(kGrid, kGrid);
    for (u32 x = 0; x < kGrid; ++x) {
        if (x < kGapLo || x > kGapHi) grid.set_blocked(x, kWallZ, true);
    }
}

Entity spawn_bot(World& w, u32 team, math::Vec3 pos) {
    const Entity e = w.create();
    TransformWS t{};
    t.mtw = math::translate(pos);
    t.prev_mtw = t.mtw;
    w.add(e, t);
    w.add(e, Team{team});
    w.add(e, Health{100.0f, 100.0f});
    Weapon wp{};
    equip_weapon(wp, WeaponClass::MachineGun);  // fast hitscan
    wp.ammo = -1;          // infinite for a sustained skirmish
    wp.hit_radius = 0.7f;  // generous hitbox for reliable headless hits
    w.add(e, wp);
    w.add(e, Score{0, 0});
    w.add(e, Respawnable{pos, 1.0f});
    w.add(e, TacticalBot{static_cast<u32>(TacticalState::Patrol), kRetreatFrac});
    return e;
}

struct Result {
    std::vector<f32> sig;
    u32 total_frags = 0;
    u32 total_deaths = 0;
    bool any_retreat = false;
};

Result run_skirmish(u32 ticks) {
    World w;
    ai::GridAStar grid;
    build_grid(grid);

    const ai::GridLayout layout{1.0f, 0.0f, 0.0f, kGrid};
    const FalloffProfile falloff = weapon_spec(WeaponClass::MachineGun).falloff;

    // Team 0 spawns on the -Z side, team 1 on the +Z side; each pushes through
    // the gap toward the other side's objective cell.
    std::vector<Entity> bots;
    std::vector<ai::NavAgent> navs;
    const u32 cols[kPerTeam] = {6, 9, 11, 14, 17};
    for (u32 i = 0; i < kPerTeam; ++i) {
        bots.push_back(spawn_bot(w, 0, {static_cast<f32>(cols[i]) + 0.5f, 0.0f, 4.5f}));
        bots.push_back(spawn_bot(w, 1, {static_cast<f32>(cols[i]) + 0.5f, 0.0f, 19.5f}));
        ai::NavAgent n0{}; n0.layout = layout; n0.follower.arrival_radius_m = 0.6f;
        ai::NavAgent n1{}; n1.layout = layout; n1.follower.arrival_radius_m = 0.6f;
        navs.push_back(n0);
        navs.push_back(n1);
    }
    // Objective cell per team: through the gap to the far side.
    const u32 obj_x[2] = {11, 11};
    const u32 obj_z[2] = {20, 4};

    std::vector<TacticalState> action(bots.size(), TacticalState::Patrol);
    std::vector<Entity> dscratch, pscratch;
    bool any_retreat = false;

    for (u32 t = 0; t < ticks; ++t) {
        // ── Phase 1: read positions, decide an action, fire when engaging. ──
        for (usize i = 0; i < bots.size(); ++i) {
            const Entity e = bots[i];
            const Health* h = w.get<Health>(e);
            if (h == nullptr || h->hp <= 0.0f) continue;
            const u32 team = w.get<Team>(e)->team;
            const math::Vec3 pos = entity_pos(w, e);
            const usize bcell = ai::world_to_cell(pos, layout, kGrid);
            const u32 bx = static_cast<u32>(bcell % kGrid);
            const u32 bz = static_cast<u32>(bcell / kGrid);

            // Nearest living enemy (ascending id => deterministic tie-break).
            Entity enemy{};
            f32 best_d2 = 1.0e30f;
            math::Vec3 enemy_pos{};
            for (usize j = 0; j < bots.size(); ++j) {
                const Entity o = bots[j];
                const Health* oh = w.get<Health>(o);
                if (oh == nullptr || oh->hp <= 0.0f) continue;
                if (w.get<Team>(o)->team == team) continue;
                const math::Vec3 op = entity_pos(w, o);
                const f32 dx = op.x - pos.x, dz = op.z - pos.z;
                const f32 d2 = dx * dx + dz * dz;
                if (d2 < best_d2) { best_d2 = d2; enemy = o; enemy_pos = op; }
            }

            bool visible = false;
            if (enemy.valid() && best_d2 <= kFireRange * kFireRange) {
                const usize ecell = ai::world_to_cell(enemy_pos, layout, kGrid);
                const u32 ex = static_cast<u32>(ecell % kGrid);
                const u32 ez = static_cast<u32>(ecell / kGrid);
                visible = ai::line_of_sight(grid, bx, bz, ex, ez);
            }

            const TacticalState st =
                decide_tactical_state(h->hp, h->max_hp, visible, kRetreatFrac);
            action[i] = st;
            w.get<TacticalBot>(e)->state = static_cast<u32>(st);
            if (st == TacticalState::Retreat) any_retreat = true;

            if (st == TacticalState::Engage && enemy.valid()) {
                const math::Vec3 dir = unit_xz({enemy_pos.x - pos.x, 0.0f,
                                                enemy_pos.z - pos.z});
                if (dir.x != 0.0f || dir.z != 0.0f) {
                    fire_hitscan(w, e, pos, dir, static_cast<i64>(team),
                                 0.0f, 0, &falloff);
                }
            }
        }

        // ── Phase 2: navigate (Patrol/Retreat) and move; Engage holds. ──
        for (usize i = 0; i < bots.size(); ++i) {
            const Entity e = bots[i];
            const Health* h = w.get<Health>(e);
            if (h == nullptr || h->hp <= 0.0f) continue;
            const u32 team = w.get<Team>(e)->team;
            const math::Vec3 pos = entity_pos(w, e);
            const usize bcell = ai::world_to_cell(pos, layout, kGrid);
            const u32 bx = static_cast<u32>(bcell % kGrid);
            const u32 bz = static_cast<u32>(bcell / kGrid);

            if (action[i] == TacticalState::Engage) continue;  // hold + shoot

            u32 gx = obj_x[team], gz = obj_z[team];
            if (action[i] == TacticalState::Retreat) {
                u32 cover = 0;
                if (ai::nearest_cover_cell(grid, bx, bz, cover)) {
                    gx = cover % kGrid;
                    gz = cover / kGrid;
                }
            }
            if (navs[i].goal_x != gx || navs[i].goal_z != gz || !navs[i].has_goal) {
                ai::set_goal(navs[i], gx, gz);
            }
            const math::Vec3 steer = ai::update(navs[i], grid, pos);
            const math::Vec3 np{pos.x + steer.x * kSpeed * kDt, 0.0f,
                                pos.z + steer.z * kSpeed * kDt};
            TransformWS* xf = w.get<TransformWS>(e);
            xf->prev_mtw = xf->mtw;
            xf->mtw = math::translate(np);
        }

        tick_weapons(w, kDt);
        update_respawns(w, kDt, dscratch);
    }

    Result r;
    r.any_retreat = any_retreat;
    for (Entity e : bots) {
        const Health* h = w.get<Health>(e);
        const Score* s = w.get<Score>(e);
        const math::Vec3 p = entity_pos(w, e);
        r.sig.push_back(h->hp);
        r.sig.push_back(static_cast<f32>(s->frags));
        r.sig.push_back(p.x);
        r.sig.push_back(p.z);
        r.total_frags += s->frags;
        r.total_deaths += s->deaths;
    }
    return r;
}

}  // namespace

TEST_CASE("tactical skirmish: nav + cover + weapons + LoS compose into a fight",
          "[tactical][gameplay][ai][determinism]") {
    const Result a = run_skirmish(600);  // 10 s of combat

    // The composed loop actually produced a fight through the chokepoint.
    REQUIRE(a.total_frags > 0u);
    REQUIRE(a.total_deaths > 0u);

    // At least one bot was hurt enough to fall back to cover — the FSM's
    // Retreat branch (and CoverPoints) really fired in the integrated loop.
    REQUIRE(a.any_retreat);
}

TEST_CASE("tactical skirmish: the whole composed loop is bit-reproducible",
          "[tactical][determinism]") {
    const Result a = run_skirmish(600);
    const Result b = run_skirmish(600);
    REQUIRE(a.sig == b.sig);
    REQUIRE(a.total_frags == b.total_frags);
    REQUIRE(a.total_deaths == b.total_deaths);
    REQUIRE(a.any_retreat == b.any_retreat);
}
