// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/match_bot_match.cpp — the AI bot driver (engine/match BotMatch)
// playing a MatchSession with no human inputs. Proves that the deterministic
// combat AI generates the per-client net inputs that make a real bot-vs-bot
// match play out over the netcode + gameplay path: bots close on the nearest
// living enemy, fire when in range, and trade frags/deaths through the
// authoritative damage path. Headless + bit-deterministic (the match lane is
// lockstep): two identical BotMatch runs produce identical frags/deaths.

#include "match/BotMatch.h"

#include "gameplay/MatchRules.h"
#include "net/TickConfig.h"

#include "math/Math.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <array>
#include <span>
#include <vector>

using namespace psynder;
using namespace psynder::match;

namespace {

constexpr u32 kLatency = 3;
constexpr u32 kTotal   = 600;  // long enough for the bots to close + trade frags

// Two opposing bots, short respawn so the loop cycles within the window. The net
// world starts both at the origin; the AI steers them apart only insofar as it
// chases — they start coincident, so frame one they hold, then as the respawn
// ring scatters them they have a real distance to close. Team 0 vs team 1 (the
// default c%2 split) so each is the other's enemy.
std::array<PlayerSpawn, 2> spawns2() {
    std::array<PlayerSpawn, 2> s{};
    s[0].pos = {0.0f, 0.0f, 0.0f};
    s[0].team = 0;
    s[0].weapon_damage = 34.0f;     // 3 hits to drop 100 hp
    s[0].respawn_delay_s = 0.1f;
    s[1].pos = {5.0f, 0.0f, 0.0f};
    s[1].team = 1;
    s[1].weapon_damage = 34.0f;
    s[1].respawn_delay_s = 0.1f;
    return s;
}

// A four-bot free-for-all on two teams (0,1,0,1) seeded around a ring.
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

// Respawn ring so a frag drops the victim away from the shooter (anti-spawn-camp)
// and the two bots have a gap to close each cycle.
std::array<math::Vec3, 4> spawn_ring() {
    return std::array<math::Vec3, 4>{
        math::Vec3{-8.0f, 0.0f, -8.0f}, math::Vec3{8.0f, 0.0f, -8.0f},
        math::Vec3{-8.0f, 0.0f, 8.0f}, math::Vec3{8.0f, 0.0f, 8.0f}};
}

gameplay::MatchConfig match_cfg() {
    gameplay::MatchConfig mc{};
    mc.frag_limit = 3u;              // first to 3 frags wins
    mc.warmup_s = 8.0f / 128.0f;     // ~8 ticks of warmup (no damage)
    mc.intermission_s = 0.25f;
    return mc;
}

// Signature of a finished run: per-client frags + deaths, plus whether the match
// reached Active. Equality => bit-deterministic outcome.
struct Sig {
    std::vector<u32> frags;
    std::vector<u32> deaths;
    bool             reached_active = false;
    bool operator==(const Sig& o) const {
        return frags == o.frags && deaths == o.deaths &&
               reached_active == o.reached_active;
    }
};

Sig run2() {
    const net::TickConfig cfg = net::tick_config_128();
    const auto sp = spawns2();
    BotMatch bm(cfg, 2u, kLatency, std::span<const PlayerSpawn>(sp));
    const auto ring = spawn_ring();
    bm.configure_match(match_cfg(), std::span<const math::Vec3>(ring));

    Sig s;
    for (u32 t = 0; t < kTotal; ++t) {
        bm.advance();
        if (bm.match_phase() == gameplay::MatchPhase::Active) s.reached_active = true;
    }
    for (u32 c = 0; c < 2u; ++c) {
        s.frags.push_back(bm.frags(c));
        s.deaths.push_back(bm.deaths(c));
    }
    return s;
}

}  // namespace

TEST_CASE("match bot driver plays a real bot-vs-bot match", "[match]") {
    const net::TickConfig cfg = net::tick_config_128();
    const auto sp = spawns2();
    BotMatch bm(cfg, 2u, kLatency, std::span<const PlayerSpawn>(sp));
    const auto ring = spawn_ring();
    bm.configure_match(match_cfg(), std::span<const math::Vec3>(ring));

    bool reached_active = false;
    for (u32 t = 0; t < kTotal; ++t) {
        bm.advance();
        if (bm.match_phase() == gameplay::MatchPhase::Active) reached_active = true;
    }

    // The match left warmup and went live.
    REQUIRE(reached_active);

    // The bots actually fought: hits flowed through the gameplay damage path and
    // somebody scored frags / took deaths.
    REQUIRE(bm.hits_applied() > 0u);
    const u32 total_frags = bm.frags(0) + bm.frags(1);
    const u32 total_deaths = bm.deaths(0) + bm.deaths(1);
    REQUIRE(total_frags > 0u);
    REQUIRE(total_deaths > 0u);
}

TEST_CASE("match bot driver runs a four-bot free-for-all", "[match]") {
    const net::TickConfig cfg = net::tick_config_128();
    const auto sp = spawns4();
    BotMatch bm(cfg, 4u, kLatency, std::span<const PlayerSpawn>(sp));
    const auto ring = spawn_ring();
    bm.configure_match(match_cfg(), std::span<const math::Vec3>(ring));

    bool reached_active = false;
    for (u32 t = 0; t < kTotal; ++t) {
        bm.advance();
        if (bm.match_phase() == gameplay::MatchPhase::Active) reached_active = true;
    }

    REQUIRE(reached_active);
    REQUIRE(bm.hits_applied() > 0u);
    u32 total_frags = 0u;
    u32 total_deaths = 0u;
    for (u32 c = 0; c < 4u; ++c) {
        total_frags += bm.frags(c);
        total_deaths += bm.deaths(c);
    }
    REQUIRE(total_frags > 0u);
    REQUIRE(total_deaths > 0u);
}

TEST_CASE("match bot driver match is bit-deterministic", "[match]") {
    const Sig a = run2();
    const Sig b = run2();
    REQUIRE(a == b);
    // And the determinism assertion is meaningful: the run did progress.
    REQUIRE(a.reached_active);
    u32 total = 0u;
    for (const u32 f : a.frags) total += f;
    REQUIRE(total > 0u);
}
