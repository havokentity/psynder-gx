// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/match_session.cpp — the netcode loop driving REAL gameplay
// (engine/match). Proves the server-authoritative bridge end to end: client
// inputs (move + fire) cross the latency channel, the server applies
// lag-compensated hit detection, and MatchSession runs those hits through the
// actual gameplay damage path (Health -> Dead -> Score + respawn) on a
// scene::World. This is DoD §8 bullet 1 ("the net loop driving real gameplay"),
// headless + deterministic.

#include "match/MatchSession.h"

#include "net/Prediction.h"
#include "net/TickConfig.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <array>
#include <span>
#include <vector>

using namespace psynder;
using namespace psynder::match;
using net::Input;
using net::kInputBtnFire;

namespace {

constexpr u32 kClients = 2;
constexpr u32 kLatency = 3;
constexpr u32 kSettle = 16;   // ticks for the victim to run onto the fire axis
constexpr u32 kTotal = 320;   // long enough for several kill/respawn cycles

// Two players, short respawn so the loop cycles within the test window.
std::array<PlayerSpawn, kClients> spawns() {
    std::array<PlayerSpawn, kClients> s{};
    s[0].pos = {0.0f, 0.0f, 0.0f};
    s[0].weapon_damage = 34.0f;   // 3 hits to drop 100 hp
    s[1].pos = {5.0f, 0.0f, 0.0f};
    s[1].respawn_delay_s = 0.1f;  // ~13 ticks at 128 Hz
    return s;
}

// Client 0 sits at the net origin aiming +X (yaw 90) and fires once the victim
// has settled on the axis; client 1 runs +X onto the axis then idles there so
// every shot connects. (Net positions start at the origin regardless of the ECS
// spawn, so the victim is driven onto the axis via move inputs.)
Input input_for(u32 client, u32 tick) {
    Input in{};
    if (client == 0) {
        in.yaw_deg = 90.0f;
        if (tick >= kSettle + kLatency) in.buttons = kInputBtnFire;
    } else {
        in.move[0] = (tick < kSettle) ? 40.0f : 0.0f;  // ~5 m then hold on axis
    }
    return in;
}

struct Sig {
    std::vector<f32> v;
    bool operator==(const Sig& o) const { return v == o.v; }
};

Sig run() {
    const net::TickConfig cfg = net::tick_config_128();
    const auto sp = spawns();
    MatchSession m(cfg, kClients, kLatency, std::span<const PlayerSpawn>(sp));
    for (u32 t = 0; t < kTotal; ++t) {
        std::array<Input, kClients> ins;
        for (u32 c = 0; c < kClients; ++c) ins[c] = input_for(c, t);
        m.advance(std::span<const Input>(ins.data(), kClients));
    }
    Sig s;
    for (u32 c = 0; c < kClients; ++c) {
        s.v.push_back(m.health(c));
        s.v.push_back(static_cast<f32>(m.frags(c)));
        s.v.push_back(static_cast<f32>(m.deaths(c)));
    }
    return s;
}

}  // namespace

TEST_CASE("match: the net loop drives real gameplay — server-auth damage + score",
          "[match][net][gameplay]") {
    const net::TickConfig cfg = net::tick_config_128();
    const auto sp = spawns();
    MatchSession m(cfg, kClients, kLatency, std::span<const PlayerSpawn>(sp));

    for (u32 t = 0; t < kTotal; ++t) {
        std::array<Input, kClients> ins;
        for (u32 c = 0; c < kClients; ++c) ins[c] = input_for(c, t);
        m.advance(std::span<const Input>(ins.data(), kClients));
    }

    // The lag-comp hits flowed through the gameplay damage path.
    REQUIRE(m.hits_applied() > 0u);
    // Client 0 fragged client 1 across at least one death/respawn cycle.
    REQUIRE(m.frags(0) >= 1u);
    REQUIRE(m.deaths(1) >= 1u);
    // The shooter was never targeted: full health, no deaths, no frags charged
    // against it.
    REQUIRE(m.alive(0));
    REQUIRE(m.health(0) == Catch::Approx(100.0f));
    REQUIRE(m.deaths(0) == 0u);
    // Frags are only credited to the killer (not self-inflicted).
    REQUIRE(m.frags(1) == 0u);
}

TEST_CASE("match: the bridged net+gameplay loop is bit-deterministic",
          "[match][determinism]") {
    const Sig a = run();
    const Sig b = run();
    REQUIRE(a == b);
}
