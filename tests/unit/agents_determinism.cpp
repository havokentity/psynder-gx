// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/agents_determinism.cpp — DOTS mass-agent golden test (ADR-019 cl.1).
//
// The steering system is on the 128-tick lockstep path, so it must be
// bit-reproducible. Agents are ordinary ECS entities (scene::World); this suite
// spawns a deterministic flock, runs a fixed tick budget at dt = 1/120, and
// asserts:
//   (a) cross-run determinism — the SAME setup run twice yields BIT-IDENTICAL
//       final TransformWS bytes (memcmp);
//   (b) progress — agents get measurably closer to their targets;
//   (c) avoidance — settled agents keep a minimum pairwise spacing;
//   (d) no NaN/inf anywhere in the final state.
//
// Tagged [agents][determinism] so the determinism CI regex
// (golden|determinism|deterministic|replay|lockstep) selects it.

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstring>
#include <vector>

#include "jobs/JobSystem.h"
#include "math/Math.h"
#include "physics/agents/AgentComponents.h"
#include "physics/agents/AgentSystem.h"
#include "scene/GxComponents.h"
#include "scene/World.h"

using namespace psynder;
using namespace psynder::physics::agents;

namespace {

constexpr u32 kAgents = 64;
constexpr u32 kTicks = 240;
constexpr f32 kDt = 1.0f / 120.0f;  // fixed 120 Hz sim tick (CharacterSpine)

// RAII pool guard: parallel_for is synchronous either way, but starting the real
// pool exercises the multi-thread path so we prove thread-count independence.
struct Pool {
    Pool() { jobs::JobSystem::Get().start(); }
    ~Pool() { jobs::JobSystem::Get().stop(); }
};

// Deterministic flock: 8x8 agents on a grid in the XZ plane, all seeking the
// origin, so they converge and MUST avoid one another on the way in. Returns the
// spawned entities in a stable order for read-back.
std::vector<Entity> make_flock(scene::World& w) {
    std::vector<Entity> out;
    out.reserve(kAgents);
    const u32 side = 8;
    const f32 spacing = 2.0f;
    for (u32 ix = 0; ix < side; ++ix) {
        for (u32 iz = 0; iz < side; ++iz) {
            scene::Agent a{};
            a.max_speed_mps = 3.0f;   // ~jog
            a.max_force = 8.0f;       // m/s^2
            a.radius_m = 0.4f;        // ~human capsule radius
            a.arrive_radius_m = 1.5f;
            const f32 x = (static_cast<f32>(ix) - 3.5f) * spacing + 12.0f;
            const f32 z = (static_cast<f32>(iz) - 3.5f) * spacing + 12.0f;
            out.push_back(scene::spawn_agent(w, a, {x, 0.0f, z}, {0.0f, 0.0f, 0.0f}));
        }
    }
    return out;
}

math::Vec3 pos_of(const scene::TransformWS& t) {
    return {t.mtw.m[12], t.mtw.m[13], t.mtw.m[14]};
}

f32 dist(math::Vec3 a, math::Vec3 b) {
    const math::Vec3 d = math::sub(a, b);
    return std::sqrt(math::dot(d, d));
}

// Run `ticks` ticks of the open-field flock; return the final TransformWS array
// (read back from the ECS in spawn order) for byte comparison.
std::vector<scene::TransformWS> run(u32 ticks) {
    scene::World w;
    const std::vector<Entity> ents = make_flock(w);
    AgentScratch scratch;
    for (u32 t = 0; t < ticks; ++t) update_agents(w, scratch, kDt);
    std::vector<scene::TransformWS> out;
    out.reserve(ents.size());
    for (Entity e : ents) out.push_back(*w.get<scene::TransformWS>(e));
    return out;
}

}  // namespace

TEST_CASE("agents same setup runs bit identical", "[agents][determinism]") {
    Pool pool;
    const std::vector<scene::TransformWS> a = run(kTicks);
    const std::vector<scene::TransformWS> b = run(kTicks);
    REQUIRE(a.size() == kAgents);
    REQUIRE(b.size() == a.size());
    REQUIRE(std::memcmp(a.data(), b.data(), a.size() * sizeof(scene::TransformWS)) == 0);
}

TEST_CASE("agents make progress toward target", "[agents][determinism]") {
    Pool pool;
    scene::World w;
    const std::vector<Entity> ents = make_flock(w);
    std::vector<f32> d0(ents.size());
    for (usize i = 0; i < ents.size(); ++i) {
        const math::Vec3 goal = w.get<scene::AgentTarget>(ents[i])->goal;
        d0[i] = dist(pos_of(*w.get<scene::TransformWS>(ents[i])), goal);
    }

    AgentScratch scratch;
    for (u32 t = 0; t < kTicks; ++t) update_agents(w, scratch, kDt);

    for (usize i = 0; i < ents.size(); ++i) {
        const math::Vec3 goal = w.get<scene::AgentTarget>(ents[i])->goal;
        const f32 d1 = dist(pos_of(*w.get<scene::TransformWS>(ents[i])), goal);
        REQUIRE(d1 < d0[i]);
    }
}

TEST_CASE("agents avoid overlapping after settling", "[agents][determinism]") {
    Pool pool;
    scene::World w;
    const std::vector<Entity> ents = make_flock(w);
    AgentScratch scratch;
    for (u32 t = 0; t < kTicks; ++t) update_agents(w, scratch, kDt);

    // Separation is a soft steering force, not a hard constraint, so require the
    // minimum pairwise spacing to stay above half the touch distance (2*radius).
    const f32 radius = 0.4f;
    const f32 min_allowed = 0.5f * (2.0f * radius);
    std::vector<math::Vec3> p(ents.size());
    for (usize i = 0; i < ents.size(); ++i) p[i] = pos_of(*w.get<scene::TransformWS>(ents[i]));
    f32 min_d = 1e30f;
    for (usize i = 0; i < p.size(); ++i)
        for (usize j = i + 1; j < p.size(); ++j)
            min_d = std::min(min_d, dist(p[i], p[j]));
    REQUIRE(min_d >= min_allowed);
}

TEST_CASE("agents produce no NaN in final state", "[agents][determinism]") {
    Pool pool;
    const std::vector<scene::TransformWS> a = run(kTicks);
    for (const scene::TransformWS& t : a)
        for (int i = 0; i < 16; ++i) REQUIRE(std::isfinite(t.mtw.m[i]));
}
