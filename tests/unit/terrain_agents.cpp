// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/terrain_agents.cpp — bind ECS movers to the heightfield
// (engine/world/outdoor/TerrainAgents). A GroundClamp entity is snapped to the
// terrain surface; combined with the DOTS agent steering, agents walk over an
// outdoor Battlefield-light heightfield staying grounded — deterministically.

#include "world/outdoor/TerrainAgents.h"
#include "world/outdoor/HeightfieldQuery.h"
#include "world/outdoor/Terrain.h"

#include "physics/agents/AgentComponents.h"
#include "physics/agents/AgentSystem.h"

#include "scene/GxComponents.h"
#include "scene/World.h"

#include "math/Math.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <vector>

using namespace psynder;
using namespace psynder::world::outdoor;
using psynder::scene::TransformWS;
using psynder::scene::World;

namespace {
// Ramp heightmap: height(x) = x metres (heights[x] = x*100, scale 0.01).
std::vector<u16> ramp(u32 sx, u32 sz) {
    std::vector<u16> hm(static_cast<usize>(sx) * sz, 0u);
    for (u32 z = 0; z < sz; ++z)
        for (u32 x = 0; x < sx; ++x) hm[z * sx + x] = static_cast<u16>(x * 100u);
    return hm;
}
HeightmapDesc desc_of(const std::vector<u16>& hm, u32 sx, u32 sz) {
    HeightmapDesc d{};
    d.size_x = sx;
    d.size_z = sz;
    d.spacing = 1.0f;
    d.height_scale = 0.01f;
    d.heights = hm.data();
    return d;
}
Entity spawn_clamped(World& w, f32 x, f32 z, f32 foot) {
    const Entity e = w.create();
    TransformWS t{};
    t.mtw = math::translate({x, 99.0f, z});  // bogus Y; the clamp fixes it
    t.prev_mtw = t.mtw;
    w.add(e, t);
    w.add(e, GroundClamp{foot});
    return e;
}
f32 ypos(World& w, Entity e) { return w.get<TransformWS>(e)->mtw.m[13]; }
}  // namespace

TEST_CASE("terrain-agents: GroundClamp snaps entities onto the surface",
          "[terrain][gameplay]") {
    const auto hm = ramp(16, 16);
    const HeightmapDesc d = desc_of(hm, 16, 16);
    World w;
    const Entity a = spawn_clamped(w, 2.0f, 4.0f, 0.5f);
    const Entity b = spawn_clamped(w, 6.0f, 4.0f, 0.5f);
    const Entity c = spawn_clamped(w, 4.0f, 4.0f, 0.0f);

    apply_terrain_clamp(w, d);

    REQUIRE(ypos(w, a) == Catch::Approx(2.5f));  // height 2 + foot 0.5
    REQUIRE(ypos(w, b) == Catch::Approx(6.5f));  // height 6 + foot 0.5
    REQUIRE(ypos(w, c) == Catch::Approx(4.0f));  // height 4 + foot 0
}

TEST_CASE("terrain-agents: a mover dragged across XZ tracks the surface",
          "[terrain][gameplay]") {
    const auto hm = ramp(16, 16);
    const HeightmapDesc d = desc_of(hm, 16, 16);
    World w;
    const Entity e = spawn_clamped(w, 0.0f, 8.0f, 0.3f);
    for (u32 step = 1; step <= 10; ++step) {
        const f32 x = static_cast<f32>(step);
        w.get<TransformWS>(e)->mtw.m[12] = x;  // "move" in +X
        apply_terrain_clamp(w, d);
        REQUIRE(ypos(w, e) == Catch::Approx(terrain_height(d, x, 8.0f) + 0.3f));
    }
}

TEST_CASE("terrain-agents: DOTS agents walk the hills staying grounded",
          "[terrain][gameplay][determinism]") {
    const auto run = []() {
        std::vector<u16> hm;
        generate_hills(hm, 48, 48, /*seed=*/3u, /*amplitude=*/1500.0f);
        HeightmapDesc d{};
        d.size_x = 48;
        d.size_z = 48;
        d.spacing = 1.0f;
        d.height_scale = 0.01f;
        d.heights = hm.data();

        World w;
        std::vector<Entity> agents;
        for (u32 i = 0; i < 8; ++i) {
            const Entity e = w.create();
            TransformWS t{};
            t.mtw = math::translate({5.0f + static_cast<f32>(i), 0.0f, 5.0f});
            t.prev_mtw = t.mtw;
            w.add(e, t);
            scene::Agent ag{};
            ag.max_speed_mps = 4.0f;
            ag.max_force = 12.0f;
            ag.radius_m = 0.5f;
            ag.arrive_radius_m = 1.0f;
            ag.height_m = 0.0f;
            w.add(e, ag);
            w.add(e, scene::AgentVelocity{{0, 0, 0}});
            w.add(e, scene::AgentTarget{{38.0f, 0.0f, 38.0f}});  // far corner
            w.add(e, GroundClamp{0.9f});
            agents.push_back(e);
        }

        physics::agents::AgentScratch scratch;
        constexpr f32 dt = 1.0f / 60.0f;
        for (u32 tick = 0; tick < 120; ++tick) {
            physics::agents::update_agents(w, scratch, dt);  // XZ steering
            apply_terrain_clamp(w, d);                        // snap to terrain
        }

        // Every agent sits exactly on the surface and has advanced toward goal.
        std::vector<f32> sig;
        bool advanced = false;
        for (Entity e : agents) {
            const TransformWS* t = w.get<TransformWS>(e);
            const f32 x = t->mtw.m[12], y = t->mtw.m[13], z = t->mtw.m[14];
            REQUIRE(y == Catch::Approx(terrain_height(d, x, z) + 0.9f).margin(1e-4f));
            if (x > 6.0f || z > 6.0f) advanced = true;
            sig.push_back(x);
            sig.push_back(y);
            sig.push_back(z);
        }
        REQUIRE(advanced);
        return sig;
    };
    REQUIRE(run() == run());  // bit-deterministic outdoor skirmish
}
