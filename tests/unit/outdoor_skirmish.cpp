// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/outdoor_skirmish.cpp — the Battlefield-light outdoor combat loop,
// integrated and headless. Two teams of flow-field combat bots spawn on a
// procedural heightfield, steer toward the centre, stay clamped to the terrain,
// shoot each other through the weapon/health systems, die + respawn, and the
// match runs to a frag limit via MatchRules — all deterministic. The outdoor
// analog of arena_combat, composing engine/world/outdoor (HeightfieldQuery +
// TerrainAgents) + engine/ai (FlowField) + engine/physics/agents + engine/
// gameplay (CombatBot / Weapons / Damage / MatchRules).

#include "world/outdoor/HeightfieldQuery.h"
#include "world/outdoor/TerrainAgents.h"
#include "world/outdoor/Terrain.h"

#include "ai/FlowField.h"

#include "gameplay/CombatBot.h"
#include "gameplay/Damage.h"
#include "gameplay/GameplayComponents.h"
#include "gameplay/MatchRules.h"
#include "gameplay/Weapons.h"

#include "physics/agents/AgentComponents.h"
#include "physics/agents/AgentSystem.h"

#include "scene/GxComponents.h"
#include "scene/World.h"

#include "math/Math.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <vector>

using namespace psynder;
using namespace psynder::gameplay;
using psynder::scene::TransformWS;
using psynder::scene::World;
namespace wo = psynder::world::outdoor;

namespace {
constexpr u32 kPerTeam = 12;
constexpr f32 kDt = 1.0f / 120.0f;
constexpr u32 kGrid = 32;
constexpr f32 kCenter = 16.0f;
constexpr f32 kFootOffset = 0.9f;

struct Result {
    std::vector<f32> sig;
    u32 total_frags = 0;
    u32 total_deaths = 0;
    bool grounded_ok = true;
    u32 round = 0;
};

Entity spawn_bot(World& w, u32 team, math::Vec3 pos) {
    const Entity e = w.create();
    TransformWS t{};
    t.mtw = math::translate(pos);
    t.prev_mtw = t.mtw;
    w.add(e, t);
    w.add(e, Team{team});
    w.add(e, Bot{14.0f, 1.5f});
    w.add(e, Health{100.0f, 100.0f});
    w.add(e, Weapon{20.0f, 0.2f, 0.0f, -1, 0.7f});  // infinite ammo
    w.add(e, Score{0u, 0u});
    w.add(e, Respawnable{pos, 1.0f});
    scene::Agent a{};
    a.max_speed_mps = 4.0f;
    a.max_force = 12.0f;
    a.radius_m = 0.5f;
    a.arrive_radius_m = 1.0f;
    a.height_m = 0.0f;
    w.add(e, a);
    w.add(e, scene::AgentVelocity{{0, 0, 0}});
    w.add(e, scene::AgentTarget{pos});
    w.add(e, wo::GroundClamp{kFootOffset});
    return e;
}

Result run_skirmish(u32 ticks) {
    // Procedural hilly terrain (deterministic; the data is shared, not per-tick).
    std::vector<u16> hm;
    wo::generate_hills(hm, kGrid, kGrid, /*seed=*/11u, /*amplitude=*/1200.0f);
    wo::HeightmapDesc terrain{};
    terrain.size_x = kGrid;
    terrain.size_z = kGrid;
    terrain.spacing = 1.0f;
    terrain.height_scale = 0.01f;
    terrain.heights = hm.data();

    World w;
    ai::FlowField field;
    field.resize({0, 0, 0}, 1.0f, kGrid, kGrid);
    field.build({kCenter, 0.0f, kCenter});  // both teams converge on the centre

    std::vector<Entity> bots;
    for (u32 i = 0; i < kPerTeam; ++i) {
        const f32 z = 10.0f + static_cast<f32>(i % 4) * 0.9f;
        const f32 row = static_cast<f32>(i / 4) * 0.9f;
        bots.push_back(spawn_bot(w, 0, {6.0f + row, 0.0f, z}));    // left team
        bots.push_back(spawn_bot(w, 1, {26.0f - row, 0.0f, z}));   // right team
    }

    // Snap everyone onto the surface before the first tick.
    wo::apply_terrain_clamp(w, terrain);

    MatchState match{};
    MatchConfig cfg{};
    cfg.frag_limit = 8u;
    cfg.warmup_s = 0.0f;

    physics::agents::AgentScratch ascratch;
    std::vector<Entity> dscratch;
    for (u32 t = 0; t < ticks; ++t) {
        tick_combat_bots(w, field, kDt);              // path + shoot
        physics::agents::update_agents(w, ascratch, kDt);  // XZ steering
        wo::apply_terrain_clamp(w, terrain);          // snap to terrain
        tick_weapons(w, kDt);                          // cooldowns
        update_respawns(w, kDt, dscratch);             // deaths/respawn
        tick_match(match, w, cfg, kDt);                // rounds + win condition
        if (match.phase == MatchPhase::Intermission) break;
    }

    Result r;
    r.round = match.round;
    for (Entity e : bots) {
        const Health* h = w.get<Health>(e);
        const Score* s = w.get<Score>(e);
        const TransformWS* xf = w.get<TransformWS>(e);
        const f32 x = xf->mtw.m[12], y = xf->mtw.m[13], z = xf->mtw.m[14];
        // Every bot sits on the surface (clamp ran last among movement steps).
        if (y != Catch::Approx(wo::terrain_height(terrain, x, z) + kFootOffset)
                     .margin(1e-3f)) {
            r.grounded_ok = false;
        }
        r.sig.push_back(h->hp);
        r.sig.push_back(static_cast<f32>(s->frags));
        r.sig.push_back(x);
        r.sig.push_back(z);
        r.total_frags += s->frags;
        r.total_deaths += s->deaths;
    }
    return r;
}
}  // namespace

TEST_CASE("outdoor: two bot teams fight a terrain-clamped deterministic skirmish",
          "[outdoor][gameplay][determinism]") {
    const Result a = run_skirmish(1200);

    // The integrated outdoor loop actually fought.
    REQUIRE(a.total_frags > 0u);
    REQUIRE(a.total_deaths > 0u);
    // Bots never left the terrain surface.
    REQUIRE(a.grounded_ok);
    // The match advanced into a live round.
    REQUIRE(a.round >= 1u);

    // ...and it is bit-reproducible: the whole outdoor stack is deterministic.
    const Result b = run_skirmish(1200);
    REQUIRE(a.sig == b.sig);
    REQUIRE(a.total_frags == b.total_frags);
}
