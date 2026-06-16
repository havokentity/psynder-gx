// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/match_rules.cpp — deterministic match orchestration
// (engine/gameplay/MatchRules): the Warmup->Active->Intermission phase machine,
// frag-limit and time-limit win conditions, frag-leader tie breaks, and
// farthest-from-enemy spawn selection — all deterministic.

#include "gameplay/MatchRules.h"
#include "gameplay/GameplayComponents.h"

#include "scene/GxComponents.h"
#include "scene/World.h"

#include "math/Math.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <array>
#include <span>
#include <vector>

using namespace psynder;
using namespace psynder::gameplay;
using psynder::scene::TransformWS;
using psynder::scene::World;

namespace {
constexpr f32 kDt = 1.0f / 128.0f;

Entity spawn_player(World& w, f32 x, u32 frags, f32 hp = 100.0f) {
    const Entity e = w.create();
    w.add(e, Score{frags, 0u});
    w.add(e, Health{hp, 100.0f});
    TransformWS t{};
    t.mtw = math::translate({x, 0.0f, 0.0f});
    t.prev_mtw = t.mtw;
    w.add(e, t);
    return e;
}
}  // namespace

TEST_CASE("match: warmup elapses into an active round", "[match][gameplay]") {
    World w;
    spawn_player(w, 0.0f, 0u);
    MatchState m{};  // starts in Warmup
    MatchConfig cfg{};
    cfg.warmup_s = 1.0f;

    // Before warmup elapses we stay in Warmup.
    for (int i = 0; i < 100; ++i) tick_match(m, w, cfg, kDt);
    REQUIRE(m.phase == MatchPhase::Warmup);
    REQUIRE(m.round == 0u);

    // After a full second (128 ticks total) we go Active, round 1.
    for (int i = 0; i < 40; ++i) tick_match(m, w, cfg, kDt);
    REQUIRE(m.phase == MatchPhase::Active);
    REQUIRE(m.round == 1u);
}

TEST_CASE("match: reaching the frag limit ends the match with that winner",
          "[match][gameplay]") {
    World w;
    const Entity a = spawn_player(w, 0.0f, 3u);   // at the limit
    spawn_player(w, 5.0f, 1u);                     // trailing
    MatchState m{};
    m.phase = MatchPhase::Active;
    m.round = 1u;
    MatchConfig cfg{};
    cfg.frag_limit = 3u;

    tick_match(m, w, cfg, kDt);
    REQUIRE(m.just_ended);
    REQUIRE(m.phase == MatchPhase::Intermission);
    REQUIRE(m.winner.raw == a.raw);
}

TEST_CASE("match: the time limit ends the match for the frag leader",
          "[match][gameplay]") {
    World w;
    spawn_player(w, 0.0f, 2u);
    const Entity b = spawn_player(w, 5.0f, 5u);  // leader
    MatchState m{};
    m.phase = MatchPhase::Active;
    MatchConfig cfg{};
    cfg.time_limit_s = 0.5f;

    bool ended = false;
    for (int i = 0; i < 100 && !ended; ++i) {
        tick_match(m, w, cfg, kDt);
        ended = m.just_ended;
    }
    REQUIRE(ended);
    REQUIRE(m.phase == MatchPhase::Intermission);
    REQUIRE(m.winner.raw == b.raw);
    REQUIRE(m.match_time_s >= 0.5f);
}

TEST_CASE("match: frag leader breaks ties by lowest entity id", "[match][gameplay]") {
    World w;
    const Entity a = spawn_player(w, 0.0f, 2u);  // created first => lower id
    spawn_player(w, 1.0f, 2u);                   // same frags, higher id
    spawn_player(w, 2.0f, 1u);
    u32 frags = 0;
    const Entity leader = frag_leader(w, &frags);
    REQUIRE(leader.raw == a.raw);
    REQUIRE(frags == 2u);
}

TEST_CASE("match: spawn selection picks the point farthest from living enemies",
          "[match][gameplay]") {
    World w;
    const Entity self = spawn_player(w, 100.0f, 0u);  // the respawner (ignored)
    spawn_player(w, 0.0f, 0u);                          // living enemy at x=0
    const std::array<math::Vec3, 3> spawns{
        math::Vec3{0.0f, 0.0f, 0.0f}, math::Vec3{10.0f, 0.0f, 0.0f},
        math::Vec3{20.0f, 0.0f, 0.0f}};
    // Farthest from the enemy at x=0 is index 2 (x=20).
    REQUIRE(select_spawn(w, std::span<const math::Vec3>(spawns), self) == 2u);

    // A DEAD enemy parked on the far spawn must not repel — still picks index 2.
    spawn_player(w, 20.0f, 0u, /*hp=*/0.0f);
    REQUIRE(select_spawn(w, std::span<const math::Vec3>(spawns), self) == 2u);
}

TEST_CASE("match: orchestration is bit-deterministic across runs",
          "[match][determinism]") {
    const auto run = []() {
        World w;
        const Entity a = spawn_player(w, 0.0f, 0u);
        const Entity b = spawn_player(w, 5.0f, 0u);
        MatchState m{};
        MatchConfig cfg{};
        cfg.warmup_s = 0.25f;
        cfg.frag_limit = 5u;
        cfg.intermission_s = 0.25f;
        std::vector<f32> trace;
        for (int i = 0; i < 400; ++i) {
            // Deterministic scoring: a frags every 20 ticks, b every 30, once live.
            if (m.phase == MatchPhase::Active) {
                if (i % 20 == 0) ++w.get<Score>(a)->frags;
                if (i % 30 == 0) ++w.get<Score>(b)->frags;
            }
            tick_match(m, w, cfg, kDt);
            trace.push_back(static_cast<f32>(static_cast<u32>(m.phase)));
            trace.push_back(static_cast<f32>(m.round));
            trace.push_back(static_cast<f32>(m.winner.raw));
            trace.push_back(m.match_time_s);
        }
        return trace;
    };
    REQUIRE(run() == run());
}
