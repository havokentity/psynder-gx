// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/arena_combat.cpp — the FPS stack integrated as a game: two teams of
// flow-field combat bots steer through an arena, shoot each other through the
// weapon/health systems, die + respawn, and score — all deterministic. This is
// the headless heart of the Quake-arena milestone (the visual BSP arena render
// is a follow-up). Exercises engine/ai (FlowField) + engine/physics/agents
// (update_agents) + engine/gameplay (CombatBot/Weapons/Damage/Score) together.

#include "ai/FlowField.h"
#include "gameplay/CombatBot.h"
#include "gameplay/Damage.h"
#include "gameplay/GameplayComponents.h"
#include "gameplay/Weapons.h"
#include "physics/agents/AgentComponents.h"
#include "physics/agents/AgentSystem.h"

#include "scene/GxComponents.h"
#include "scene/SceneComponents.h"
#include "scene/World.h"

#include "math/Math.h"

#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace psynder;
using namespace psynder::gameplay;
using psynder::scene::TransformWS;
using psynder::scene::World;

namespace {

constexpr u32 kPerTeam = 16;
constexpr f32 kDt = 1.0f / 120.0f;
constexpr f32 kGoalX = 15.0f, kGoalZ = 15.0f;

Entity spawn_arena_bot(World& w, u32 team, math::Vec3 pos) {
    const Entity e = w.create();
    TransformWS t{};
    t.mtw = math::translate(pos);
    t.prev_mtw = t.mtw;
    w.add(e, t);
    w.add(e, Team{team});
    w.add(e, Bot{12.0f, 1.5f});
    w.add(e, Health{100.0f, 100.0f});
    w.add(e, Weapon{18.0f, 0.25f, 0.0f, -1, 0.6f});  // infinite ammo, big hitbox
    w.add(e, Score{0, 0});
    w.add(e, Respawnable{pos, 1.0f});
    // Agent movement (the steering moves it toward the AgentTarget the bot sets).
    scene::Agent a{};
    a.max_speed_mps = 4.0f;
    a.max_force = 12.0f;
    a.radius_m = 0.5f;
    a.arrive_radius_m = 1.0f;
    a.height_m = 0.0f;
    w.add(e, a);
    w.add(e, scene::AgentVelocity{{0, 0, 0}});
    w.add(e, scene::AgentTarget{pos});
    return e;
}

// Run the arena sim for `ticks` and return a deterministic signature
// (per-bot hp + frags + x,z) plus aggregate combat counters.
struct Result {
    std::vector<f32> sig;
    u32 total_frags = 0;
    u32 total_deaths = 0;
};

Result run_arena(u32 ticks) {
    World w;
    ai::FlowField field;
    field.resize({0, 0, 0}, 1.0f, 30, 30);
    field.build({kGoalX, 0.0f, kGoalZ});  // both teams converge on the centre

    std::vector<Entity> bots;
    for (u32 i = 0; i < kPerTeam; ++i) {
        const f32 z = 12.0f + static_cast<f32>(i % 4) * 0.8f;
        const f32 row = static_cast<f32>(i / 4) * 0.8f;
        bots.push_back(spawn_arena_bot(w, 0, {9.0f + row, 0.0f, z}));   // left team
        bots.push_back(spawn_arena_bot(w, 1, {21.0f - row, 0.0f, z}));  // right team
    }

    physics::agents::AgentScratch ascratch;
    physics::agents::StaticColliders no_statics{};  // open arena floor
    std::vector<Entity> dscratch, pscratch;
    for (u32 t = 0; t < ticks; ++t) {
        tick_combat_bots(w, field, kDt);                       // target + fire
        physics::agents::update_agents(w, no_statics, ascratch, kDt);  // move
        tick_weapons(w, kDt);                                  // cooldowns
        tick_projectiles(w, kDt, pscratch);                    // (no projectiles here)
        update_respawns(w, kDt, dscratch);                     // death timers
    }

    Result r;
    for (Entity e : bots) {
        const Health* h = w.get<Health>(e);
        const Score* s = w.get<Score>(e);
        const TransformWS* xf = w.get<TransformWS>(e);
        r.sig.push_back(h->hp);
        r.sig.push_back(static_cast<f32>(s->frags));
        r.sig.push_back(xf->mtw.m[12]);
        r.sig.push_back(xf->mtw.m[14]);
        r.total_frags += s->frags;
        r.total_deaths += s->deaths;
    }
    return r;
}

}  // namespace

TEST_CASE("arena: two flow-field bot teams fight a full deterministic combat loop",
          "[arena][gameplay][determinism]") {
    const Result a = run_arena(360);  // ~3 s of combat

    // The integrated loop actually produced a fight: kills + deaths happened.
    REQUIRE(a.total_frags > 0u);
    REQUIRE(a.total_deaths > 0u);

    // ...and it is bit-reproducible (the whole stack is deterministic).
    const Result b = run_arena(360);
    REQUIRE(a.sig == b.sig);
    REQUIRE(a.total_frags == b.total_frags);
}
