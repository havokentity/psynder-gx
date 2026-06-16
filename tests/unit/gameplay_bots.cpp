// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/gameplay_bots.cpp — combat bots path via the flow field and shoot
// in-range enemies through the weapon systems; deterministic at scale.

#include "ai/FlowField.h"
#include "gameplay/CombatBot.h"
#include "gameplay/GameplayComponents.h"
#include "gameplay/Weapons.h"
#include "physics/agents/AgentComponents.h"

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

namespace {
TransformWS at(math::Vec3 p) {
    TransformWS t{};
    t.mtw = math::translate(p);
    t.prev_mtw = t.mtw;
    return t;
}
Entity spawn_bot(World& w, u32 team, math::Vec3 pos, f32 range) {
    const Entity e = w.create();
    w.add(e, Team{team});
    w.add(e, Bot{range, 1.0f});
    w.add(e, Health{100.0f, 100.0f});
    w.add(e, Weapon{25.0f, 0.5f, 0.0f, 1000, 1.0f});
    w.add(e, scene::AgentTarget{{0, 0, 0}});
    w.add(e, at(pos));
    return e;
}
Entity spawn_enemy(World& w, u32 team, math::Vec3 pos) {
    const Entity e = w.create();
    w.add(e, Team{team});
    w.add(e, Health{100.0f, 100.0f});
    w.add(e, at(pos));
    return e;
}
}  // namespace

TEST_CASE("ai: a combat bot shoots an in-range enemy + paths along the field",
          "[gameplay][bots]") {
    World w;
    ai::FlowField field;
    field.resize({0, 0, 0}, 1.0f, 12, 4);
    field.build({11.0f, 0.0f, 1.0f});  // goal in +X

    const Entity bot = spawn_bot(w, /*team=*/0, {0.5f, 0.0f, 1.5f}, /*range=*/10.0f);
    const Entity foe = spawn_enemy(w, /*team=*/1, {5.5f, 0.0f, 1.5f});

    tick_combat_bots(w, field, 1.0f / 120.0f);

    REQUIRE(w.get<Health>(foe)->hp == Catch::Approx(75.0f));  // took a shot
    // Bot retargeted along the flow toward +X (goal).
    REQUIRE(w.get<scene::AgentTarget>(bot)->goal.x > 0.5f);
}

TEST_CASE("ai: a combat bot holds fire on an out-of-range enemy",
          "[gameplay][bots]") {
    World w;
    ai::FlowField field;
    field.resize({0, 0, 0}, 1.0f, 40, 4);
    field.build({39.0f, 0.0f, 1.0f});
    spawn_bot(w, 0, {0.5f, 0.0f, 1.5f}, /*range=*/8.0f);
    const Entity foe = spawn_enemy(w, 1, {30.5f, 0.0f, 1.5f});  // 30 m away
    tick_combat_bots(w, field, 1.0f / 120.0f);
    REQUIRE(w.get<Health>(foe)->hp == Catch::Approx(100.0f));  // unharmed
}

TEST_CASE("ai: combat bots are deterministic at scale", "[gameplay][bots][determinism]") {
    const auto run = []() {
        World w;
        ai::FlowField field;
        field.resize({-2, 0, -2}, 1.0f, 24, 24);
        field.build({20.0f, 0.0f, 20.0f});
        std::vector<Entity> enemies;
        for (int i = 0; i < 64; ++i) {
            const f32 x = static_cast<f32>(i % 8) * 1.5f;
            const f32 z = static_cast<f32>(i / 8) * 1.5f;
            spawn_bot(w, 0, {x, 0.0f, z}, 12.0f);
            enemies.push_back(spawn_enemy(w, 1, {x + 0.3f, 0.0f, z + 0.3f}));
        }
        constexpr f32 dt = 1.0f / 120.0f;
        for (int step = 0; step < 40; ++step) {
            tick_weapons(w, dt);
            tick_combat_bots(w, field, dt);
        }
        std::vector<f32> hp;
        for (Entity e : enemies) hp.push_back(w.get<Health>(e)->hp);
        return hp;
    };
    const std::vector<f32> a = run();
    const std::vector<f32> b = run();
    REQUIRE(a == b);
    // Bots engaged: at least some enemies took damage.
    bool any = false;
    for (f32 h : a) if (h < 100.0f) any = true;
    REQUIRE(any);
}
