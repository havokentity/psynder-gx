// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/agents_crowd_avoid_statics.cpp — proves the static push-out the
// 02_crate play-mode crowd relies on (ADR-019 class 1).
//
// The sample spawns a steering crowd that chases the player and must FLOW AROUND
// the static crates/walls. That depends on update_agents' capsule-vs-static
// push-out: agents whose straight-line path to the goal is blocked by a static
// box must not end up INSIDE that box. This suite builds a small static wall
// between a handful of agents and their shared goal, steps 240 fixed ticks at
// dt = 1/120 (the play sim tick), and asserts:
//   (a) no agent centre ends up inside any static AABB (clearance >= ~radius),
//   (b) every final TransformWS component is finite.
//
// Tagged [agents] so it rides the agents suite; not a determinism golden (that
// lives in agents_determinism.cpp).

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

#include "core/Types.h"
#include "jobs/JobSystem.h"
#include "math/Math.h"
#include "physics/agents/AgentComponents.h"
#include "physics/agents/AgentSystem.h"
#include "scene/GxComponents.h"
#include "scene/World.h"

using namespace psynder;
using namespace psynder::physics::agents;

namespace {

constexpr u32 kTicks = 240;
constexpr f32 kDt = 1.0f / 120.0f;
constexpr f32 kRadius = 0.4f;

// RAII pool guard so the parallel agent path runs.
struct Pool {
    Pool() { jobs::JobSystem::Get().start(); }
    ~Pool() { jobs::JobSystem::Get().stop(); }
};

math::Vec3 pos_of(const scene::TransformWS& t) {
    return {t.mtw.m[12], t.mtw.m[13], t.mtw.m[14]};
}

// A point is "inside" an AABB grown by `skin` on every axis.
bool inside_grown(const math::Vec3& p, const math::Aabb& b, f32 skin) {
    return p.x > b.min.x - skin && p.x < b.max.x + skin &&
           p.y > b.min.y - skin && p.y < b.max.y + skin &&
           p.z > b.min.z - skin && p.z < b.max.z + skin;
}

}  // namespace

TEST_CASE("crowd agents do not end up inside static boxes", "[agents]") {
    Pool pool;
    scene::World w;

    // A static wall of three boxes spanning x in [-3, 3], centred at z = 0,
    // standing between the agents (at z = -8) and their goal (at z = +8). The
    // only way through is to flow around the ends.
    std::vector<Entity>     static_ents;
    std::vector<math::Aabb> static_aabbs;
    const f32 half = 1.0f;  // 2 m cube
    for (int i = -1; i <= 1; ++i) {
        const f32 cx = static_cast<f32>(i) * 2.0f;
        static_ents.push_back(Entity{static_cast<u64>(1000 + i + 1)});
        static_aabbs.push_back(math::Aabb{{cx - half, 0.0f, -half},
                                          {cx + half, 2.0f * half, half}});
    }
    const StaticColliders statics{
        std::span<const Entity>(static_ents.data(), static_ents.size()),
        std::span<const math::Aabb>(static_aabbs.data(), static_aabbs.size())};

    // A handful of agents on a line behind the wall, all seeking the same goal
    // directly through the wall.
    const math::Vec3 goal{0.0f, kRadius, 8.0f};
    std::vector<Entity> agents;
    for (int i = 0; i < 6; ++i) {
        scene::Agent a{};
        a.max_speed_mps = 3.0f;
        a.max_force = 9.0f;
        a.radius_m = kRadius;
        a.arrive_radius_m = 1.5f;
        const f32 x = (static_cast<f32>(i) - 2.5f) * 1.0f;
        agents.push_back(scene::spawn_agent(w, a, {x, kRadius, -8.0f}, goal));
    }

    AgentScratch scratch;
    for (u32 t = 0; t < kTicks; ++t) {
        update_agents(w, statics, scratch, kDt);
    }

    // (b) finite everywhere.
    for (Entity e : agents) {
        const scene::TransformWS& xf = *w.get<scene::TransformWS>(e);
        for (int i = 0; i < 16; ++i) REQUIRE(std::isfinite(xf.mtw.m[i]));
    }

    // (a) no agent centre inside a static box (keep at least ~half a radius of
    // clearance — push-out is a soft force, not a hard constraint).
    const f32 clearance = 0.5f * kRadius;
    for (Entity e : agents) {
        const math::Vec3 p = pos_of(*w.get<scene::TransformWS>(e));
        for (const math::Aabb& b : static_aabbs) {
            REQUIRE_FALSE(inside_grown(p, b, clearance));
        }
    }
}
