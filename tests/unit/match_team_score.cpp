// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/match_team_score.cpp — deterministic team-deathmatch scoring
// (engine/match/TeamScore): per-team frag/death aggregation, exclusion of
// teamless players, frag totals, leading-team (lowest-index tie break), the
// TDM frag-limit win condition, and bit-for-bit determinism across recomputes.

#include "match/TeamScore.h"

#include "gameplay/GameplayComponents.h"
#include "scene/World.h"

#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace psynder;
using namespace psynder::match;
using psynder::gameplay::Score;
using psynder::gameplay::Team;
using psynder::scene::World;

namespace {

// Spawn a player on `team` with the given frags/deaths.
Entity spawn(World& w, u32 team, u32 frags, u32 deaths) {
    const Entity e = w.create();
    w.add(e, Score{frags, deaths});
    w.add(e, Team{team});
    return e;
}

}  // namespace

TEST_CASE("match: team_frags / team_deaths sum only the right team", "[match]") {
    World w;
    // Team 0: 2 + 3 frags, 1 + 0 deaths.
    spawn(w, 0u, 2u, 1u);
    spawn(w, 0u, 3u, 0u);
    // Team 1: 5 frags, 4 deaths.
    spawn(w, 1u, 5u, 4u);
    // Team 2: 1 frag, 1 death.
    spawn(w, 2u, 1u, 1u);

    REQUIRE(team_frags(w, 0u) == 5u);
    REQUIRE(team_frags(w, 1u) == 5u);
    REQUIRE(team_frags(w, 2u) == 1u);
    REQUIRE(team_deaths(w, 0u) == 1u);
    REQUIRE(team_deaths(w, 1u) == 4u);
    REQUIRE(team_deaths(w, 2u) == 1u);

    // A team with no players sums to zero.
    REQUIRE(team_frags(w, 7u) == 0u);
    REQUIRE(team_deaths(w, 7u) == 0u);
}

TEST_CASE("match: an entity with a Score but no Team is excluded", "[match]") {
    World w;
    spawn(w, 0u, 4u, 0u);
    // Teamless scorer: a Score with no Team belongs to no team total.
    const Entity loner = w.create();
    w.add(loner, Score{99u, 99u});

    REQUIRE(team_frags(w, 0u) == 4u);   // only the team-0 player counts
    REQUIRE(team_deaths(w, 0u) == 0u);

    std::vector<u32> totals;
    team_frag_totals(w, 3u, totals);
    REQUIRE(totals[0] == 4u);
    REQUIRE(totals[1] == 0u);
    REQUIRE(totals[2] == 0u);
}

TEST_CASE("match: team_frag_totals fills the vector per team", "[match]") {
    World w;
    spawn(w, 0u, 2u, 0u);
    spawn(w, 0u, 1u, 0u);
    spawn(w, 2u, 7u, 0u);  // team 1 left empty

    std::vector<u32> totals;
    // Pre-fill with garbage to prove clear+resize.
    totals.assign(9u, 123u);
    team_frag_totals(w, 3u, totals);

    REQUIRE(totals.size() == 3u);
    REQUIRE(totals[0] == 3u);
    REQUIRE(totals[1] == 0u);
    REQUIRE(totals[2] == 7u);

    // num_teams == 0 yields an empty vector.
    team_frag_totals(w, 0u, totals);
    REQUIRE(totals.empty());
}

TEST_CASE("match: leading_team picks the max and breaks ties to the low index",
          "[match]") {
    World w;
    spawn(w, 0u, 2u, 0u);
    spawn(w, 1u, 5u, 0u);  // clear leader
    spawn(w, 2u, 3u, 0u);
    REQUIRE(leading_team(w, 3u) == 1u);

    // An exact tie between team 0 and team 1 breaks to the lower index (0).
    World tie;
    spawn(tie, 0u, 4u, 0u);
    spawn(tie, 1u, 4u, 0u);
    spawn(tie, 2u, 1u, 0u);
    REQUIRE(leading_team(tie, 3u) == 0u);

    // num_teams == 0 returns 0.
    REQUIRE(leading_team(w, 0u) == 0u);
}

TEST_CASE("match: team_frag_limit_reached fires at the limit with the winner",
          "[match]") {
    World w;
    spawn(w, 0u, 3u, 0u);
    spawn(w, 1u, 7u, 0u);  // team 1 will cross the limit
    spawn(w, 2u, 2u, 0u);

    u32 winner = 0xFFFFFFFFu;  // sentinel: must be untouched when below the limit

    // Below the limit: false, out_winner untouched.
    REQUIRE_FALSE(team_frag_limit_reached(w, 3u, 8u, winner));
    REQUIRE(winner == 0xFFFFFFFFu);

    // At/above the limit: true, winner is the leading team.
    winner = 0xFFFFFFFFu;
    REQUIRE(team_frag_limit_reached(w, 3u, 7u, winner));
    REQUIRE(winner == 1u);

    // Exactly at the limit fires too.
    winner = 0xFFFFFFFFu;
    REQUIRE(team_frag_limit_reached(w, 3u, 5u, winner));
    REQUIRE(winner == 1u);
}

TEST_CASE("match: team-limit winner respects the lowest-index tie break",
          "[match]") {
    World w;
    spawn(w, 0u, 6u, 0u);  // tied leaders at 6
    spawn(w, 1u, 6u, 0u);
    u32 winner = 0xFFFFFFFFu;
    REQUIRE(team_frag_limit_reached(w, 2u, 6u, winner));
    REQUIRE(winner == 0u);  // tie -> lowest index
}

TEST_CASE("match: team scoring is bit-deterministic across recomputes",
          "[match]") {
    World w;
    spawn(w, 0u, 2u, 1u);
    spawn(w, 0u, 4u, 3u);
    spawn(w, 1u, 5u, 2u);
    spawn(w, 2u, 5u, 0u);

    std::vector<u32> a;
    std::vector<u32> b;
    team_frag_totals(w, 3u, a);
    team_frag_totals(w, 3u, b);
    REQUIRE(a == b);

    // The same world yields identical aggregate reads every time.
    REQUIRE(team_frags(w, 0u) == team_frags(w, 0u));
    REQUIRE(leading_team(w, 3u) == leading_team(w, 3u));
    u32 w1 = 0u;
    u32 w2 = 0u;
    const bool r1 = team_frag_limit_reached(w, 3u, 6u, w1);
    const bool r2 = team_frag_limit_reached(w, 3u, 6u, w2);
    REQUIRE(r1 == r2);
    REQUIRE(w1 == w2);
}
