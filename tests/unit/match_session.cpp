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

#include "gameplay/MatchRules.h"
#include "net/Prediction.h"
#include "net/TickConfig.h"

#include "math/Math.h"

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

namespace {
// Run a configured match (warmup + frag limit + spawn ring) and capture the
// outcome: phase reached, winner, round, and the victim's chosen respawn point.
struct MatchOutcome {
    bool reached_active = false;
    bool ended = false;
    u64  winner_raw = 0;
    u64  shooter_raw = 0;
    u32  round = 0;
    math::Vec3 victim_spawn{0.0f, 0.0f, 0.0f};
};

MatchOutcome run_configured() {
    const net::TickConfig cfg = net::tick_config_128();
    const auto sp = spawns();
    MatchSession m(cfg, kClients, kLatency, std::span<const PlayerSpawn>(sp));

    gameplay::MatchConfig mc{};
    mc.frag_limit = 2u;
    mc.warmup_s = 8.0f / 128.0f;  // ~8 ticks of warmup (no damage)
    mc.intermission_s = 0.25f;
    const std::array<math::Vec3, 4> pts{
        math::Vec3{-8.0f, 0.0f, -8.0f}, math::Vec3{8.0f, 0.0f, -8.0f},
        math::Vec3{-8.0f, 0.0f, 8.0f}, math::Vec3{8.0f, 0.0f, 8.0f}};
    m.configure_match(mc, std::span<const math::Vec3>(pts));

    MatchOutcome out;
    out.shooter_raw = m.player(0).raw;
    for (u32 t = 0; t < kTotal; ++t) {
        std::array<Input, kClients> ins;
        for (u32 c = 0; c < kClients; ++c) ins[c] = input_for(c, t);
        m.advance(std::span<const Input>(ins.data(), kClients));
        if (m.match_phase() == gameplay::MatchPhase::Active) out.reached_active = true;
        if (m.match().just_ended && !out.ended) {
            out.ended = true;
            out.winner_raw = m.match_winner().raw;
            out.round = m.match_round();
            // The victim (client 1) picked a respawn point on its death.
            if (const auto* r =
                    m.world().get<gameplay::Respawnable>(m.player(1))) {
                out.victim_spawn = r->spawn_pos;
            }
            break;
        }
    }
    return out;
}
}  // namespace

TEST_CASE("match: a configured session runs a real match to a frag-limit winner",
          "[match][gameplay]") {
    const MatchOutcome o = run_configured();
    REQUIRE(o.reached_active);
    REQUIRE(o.ended);
    REQUIRE(o.round == 1u);
    // The shooter (client 0) reached the frag limit first and won.
    REQUIRE(o.winner_raw == o.shooter_raw);
    // The victim's death picked one of the four spawn-ring points (not its
    // original 5,0,0 spawn) via the farthest-from-enemy heuristic.
    const bool on_ring =
        (std::abs(o.victim_spawn.x) == Catch::Approx(8.0f)) &&
        (std::abs(o.victim_spawn.z) == Catch::Approx(8.0f));
    REQUIRE(on_ring);
}

TEST_CASE("match: the configured match is bit-deterministic", "[match][determinism]") {
    const MatchOutcome a = run_configured();
    const MatchOutcome b = run_configured();
    REQUIRE(a.ended == b.ended);
    REQUIRE(a.winner_raw == b.winner_raw);
    REQUIRE(a.round == b.round);
    REQUIRE(a.victim_spawn.x == b.victim_spawn.x);
    REQUIRE(a.victim_spawn.z == b.victim_spawn.z);
}
