// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/networked_bot_match.cpp — THE CAPSTONE: a bot-vs-bot match plays
// server-authoritatively (engine/match BotMatch), its authoritative entity
// states are replicated to a client every tick through the bandwidth-managed
// pipeline (engine/net ReplicationServer/Client: priority + budget + quantize +
// fragment + playout interpolation), and the client reconstructs the match.
//
// This is the Definition-of-Done's "net loop driving real gameplay, bots
// playing it, replicated to a peer" — end to end and bit-deterministic. It
// composes the two biggest pieces of the iter-45 push (ReplicationPipeline +
// BotMatch) on top of MatchSession + the gameplay/combat systems.

#include "match/BotMatch.h"

#include "gameplay/MatchRules.h"
#include "net/ReplicationPipeline.h"
#include "net/ReplicationSession.h"  // EntityState, authoritative()
#include "net/TickConfig.h"

#include "math/Math.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <span>
#include <vector>

using namespace psynder;
using namespace psynder::match;

namespace {

constexpr u32 kLatency = 3u;
constexpr u32 kTicks = 400u;
constexpr f32 kDt = 1.0f / 128.0f;
constexpr f32 kRes = 0.05f;                  // 5 cm position quantization
constexpr f32 kInterpDelay = 3.0f * kDt;     // client trails ~3 ticks

std::array<PlayerSpawn, 4> spawns4() {
    std::array<PlayerSpawn, 4> s{};
    const std::array<math::Vec3, 4> p{
        math::Vec3{-8.0f, 0.0f, -8.0f}, math::Vec3{8.0f, 0.0f, -8.0f},
        math::Vec3{-8.0f, 0.0f, 8.0f}, math::Vec3{8.0f, 0.0f, 8.0f}};
    for (u32 c = 0; c < 4; ++c) {
        s[c].pos = p[c];
        s[c].team = c % 2u;
        s[c].weapon_damage = 34.0f;
        s[c].respawn_delay_s = 0.1f;
    }
    return s;
}

std::array<math::Vec3, 4> ring() {
    return std::array<math::Vec3, 4>{
        math::Vec3{-8.0f, 0.0f, -8.0f}, math::Vec3{8.0f, 0.0f, -8.0f},
        math::Vec3{-8.0f, 0.0f, 8.0f}, math::Vec3{8.0f, 0.0f, 8.0f}};
}

gameplay::MatchConfig mcfg() {
    gameplay::MatchConfig m{};
    m.frag_limit = 3u;
    m.warmup_s = 8.0f / 128.0f;
    m.intermission_s = 0.25f;
    return m;
}

struct Result {
    std::vector<net::EntityState> final_view;  // the client's reconstructed view
    u32   total_frags = 0;
    u32   total_deaths = 0;
    bool  reached_active = false;
    usize hits = 0;
};

Result run() {
    const net::TickConfig cfg = net::tick_config_128();
    const auto sp = spawns4();
    BotMatch bm(cfg, 4u, kLatency, std::span<const PlayerSpawn>(sp));
    const auto r = ring();
    bm.configure_match(mcfg(), std::span<const math::Vec3>(r));

    net::ReplicationServer server;
    // A budget that comfortably fits all 4 player records every tick (4*20 bytes).
    server.configure(/*entity_count=*/4u, /*bytes_per_s=*/200000.0f,
                     /*burst=*/4000u, kRes, /*mtu=*/200u);
    net::ReplicationClient client;
    client.configure(kRes, kInterpDelay);

    const std::vector<f32> prio(4, 1.0f);  // every player wants sending each tick
    std::vector<std::vector<u8>> frags;
    f64 server_time = 0.0;
    Result res;

    for (u32 t = 0; t < kTicks; ++t) {
        bm.advance();  // bots play one authoritative tick

        // Replicate the authoritative net world to the client through the pipeline.
        const std::vector<net::EntityState>& states = bm.session().net().authoritative();
        server.tick(states,
                    std::span<const f32>(prio.data(), states.size()),
                    kDt, static_cast<u16>(t), frags);
        server_time += static_cast<f64>(kDt);
        client.receive(std::span<const std::vector<u8>>(frags.data(), frags.size()),
                       server_time);
        client.advance(kDt);

        if (bm.match().phase == gameplay::MatchPhase::Active) res.reached_active = true;
    }

    client.view(res.final_view);
    for (u32 c = 0; c < 4; ++c) {
        res.total_frags += bm.frags(c);
        res.total_deaths += bm.deaths(c);
    }
    res.hits = bm.hits_applied();
    return res;
}

}  // namespace

TEST_CASE("networked bot match: a replicated bot match plays end to end",
          "[match][net]") {
    const Result r = run();

    // The server-authoritative bot match actually progressed (bots fought).
    REQUIRE(r.reached_active);
    REQUIRE(r.hits > 0u);
    REQUIRE(r.total_frags + r.total_deaths > 0u);

    // The client reconstructed every replicated player through the full pipeline.
    REQUIRE(r.final_view.size() == 4u);
    for (const net::EntityState& e : r.final_view) {
        CHECK(std::isfinite(e.pos[0]));
        CHECK(std::isfinite(e.pos[2]));
        CHECK(std::fabs(e.pos[0]) < 1000.0f);  // sane, inside the arena bounds
        CHECK(std::fabs(e.pos[2]) < 1000.0f);
    }
}

TEST_CASE("networked bot match: the whole replicated loop is deterministic",
          "[match][net][determinism]") {
    const Result a = run();
    const Result b = run();
    REQUIRE(a.total_frags == b.total_frags);
    REQUIRE(a.total_deaths == b.total_deaths);
    REQUIRE(a.final_view.size() == b.final_view.size());
    // The client's reconstructed view is bit-identical across runs.
    for (usize i = 0; i < a.final_view.size(); ++i) {
        CHECK(a.final_view[i].id == b.final_view[i].id);
        CHECK(a.final_view[i].pos[0] == b.final_view[i].pos[0]);
        CHECK(a.final_view[i].pos[2] == b.final_view[i].pos[2]);
    }
}
