// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/match_objective.cpp — deterministic capture-point objective
// (engine/match/Objective): neutral init, single-team capture + ownership flip,
// contested/empty decay, owner-standing no-recapture, sole_occupant/is_contested
// presence logic, [0,1] progress clamping, and bit-for-bit determinism.

#include "match/Objective.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <span>

using namespace psynder;
using namespace psynder::match;

namespace {

// std::span over a fixed team-count array (team t present iff counts[t] != 0).
template <usize N>
std::span<const u32> counts(const std::array<u32, N>& a) noexcept {
    return std::span<const u32>(a.data(), a.size());
}

}  // namespace

TEST_CASE("match: a fresh control point is neutral", "[match]") {
    ControlPoint cp{};
    control_point_init(cp);
    REQUIRE(cp.owner_team == kNoTeam);
    REQUIRE(cp.capturing_team == kNoTeam);
    REQUIRE(cp.progress == Catch::Approx(0.0f));
}

TEST_CASE("match: sole_occupant returns the only present team or kNoTeam",
          "[match]") {
    // Exactly one team present -> that team index.
    const std::array<u32, 3> only1{0u, 5u, 0u};
    REQUIRE(sole_occupant(counts(only1)) == 1u);

    // Two teams present -> contested -> kNoTeam.
    const std::array<u32, 3> two{2u, 0u, 1u};
    REQUIRE(sole_occupant(counts(two)) == kNoTeam);

    // Empty point -> kNoTeam.
    const std::array<u32, 3> none{0u, 0u, 0u};
    REQUIRE(sole_occupant(counts(none)) == kNoTeam);

    // Team 0 alone is a real index, not the kNoTeam sentinel.
    const std::array<u32, 3> only0{4u, 0u, 0u};
    REQUIRE(sole_occupant(counts(only0)) == 0u);
}

TEST_CASE("match: is_contested is true only with two-or-more teams", "[match]") {
    const std::array<u32, 3> empty{0u, 0u, 0u};
    const std::array<u32, 3> one{0u, 3u, 0u};
    const std::array<u32, 3> two{1u, 1u, 0u};
    const std::array<u32, 3> three{1u, 1u, 1u};
    REQUIRE_FALSE(is_contested(counts(empty)));
    REQUIRE_FALSE(is_contested(counts(one)));
    REQUIRE(is_contested(counts(two)));
    REQUIRE(is_contested(counts(three)));
}

TEST_CASE("match: a sole team captures and flips ownership at full progress",
          "[match]") {
    ControlPoint cp{};
    control_point_init(cp);

    // Team 1 alone; rate 0.25/s, dt 1s -> +0.25 progress per tick.
    const std::array<u32, 3> team1{0u, 1u, 0u};

    tick_control_point(cp, counts(team1), 0.25f, 0.5f, 1.0f);
    REQUIRE(cp.capturing_team == 1u);
    REQUIRE(cp.owner_team == kNoTeam);  // not captured yet
    REQUIRE(cp.progress == Catch::Approx(0.25f));

    tick_control_point(cp, counts(team1), 0.25f, 0.5f, 1.0f);
    REQUIRE(cp.progress == Catch::Approx(0.50f));
    tick_control_point(cp, counts(team1), 0.25f, 0.5f, 1.0f);
    REQUIRE(cp.progress == Catch::Approx(0.75f));

    // Fourth tick reaches 1.0 -> the point flips to team 1 and resets.
    tick_control_point(cp, counts(team1), 0.25f, 0.5f, 1.0f);
    REQUIRE(cp.owner_team == 1u);
    REQUIRE(cp.progress == Catch::Approx(0.0f));
    REQUIRE(cp.capturing_team == kNoTeam);
}

TEST_CASE("match: progress clamps to [0,1] on a large capture step", "[match]") {
    ControlPoint cp{};
    control_point_init(cp);
    const std::array<u32, 2> team0{1u, 0u};

    // A huge step would overshoot 1.0; the flip resets progress to exactly 0.
    tick_control_point(cp, counts(team0), 10.0f, 1.0f, 1.0f);
    REQUIRE(cp.owner_team == 0u);
    REQUIRE(cp.progress == Catch::Approx(0.0f));
    REQUIRE(cp.progress >= 0.0f);
    REQUIRE(cp.progress <= 1.0f);
}

TEST_CASE("match: a contested point does not progress and decays partials",
          "[match]") {
    ControlPoint cp{};
    control_point_init(cp);

    // Build partial progress for team 1 first (sole occupant).
    const std::array<u32, 3> team1{0u, 1u, 0u};
    tick_control_point(cp, counts(team1), 0.4f, 0.2f, 1.0f);  // -> 0.4
    REQUIRE(cp.progress == Catch::Approx(0.4f));

    // Now two teams contest it: no forward progress; the partial decays.
    const std::array<u32, 3> contest{1u, 1u, 0u};
    tick_control_point(cp, counts(contest), 0.4f, 0.2f, 1.0f);  // -> 0.2
    REQUIRE(cp.owner_team == kNoTeam);
    REQUIRE(cp.progress == Catch::Approx(0.2f));

    tick_control_point(cp, counts(contest), 0.4f, 0.2f, 1.0f);  // -> 0.0
    REQUIRE(cp.progress == Catch::Approx(0.0f));
    REQUIRE(cp.capturing_team == kNoTeam);  // cleared at zero
}

TEST_CASE("match: an empty point decays any partial progress to zero",
          "[match]") {
    ControlPoint cp{};
    control_point_init(cp);

    const std::array<u32, 3> team2{0u, 0u, 2u};
    tick_control_point(cp, counts(team2), 0.5f, 0.5f, 1.0f);  // -> 0.5
    REQUIRE(cp.progress == Catch::Approx(0.5f));
    REQUIRE(cp.capturing_team == 2u);

    // Everyone leaves: decay toward 0, then clear the capturer at 0.
    const std::array<u32, 3> empty{0u, 0u, 0u};
    tick_control_point(cp, counts(empty), 0.5f, 0.5f, 1.0f);  // -> 0.0
    REQUIRE(cp.progress == Catch::Approx(0.0f));
    REQUIRE(cp.capturing_team == kNoTeam);
    REQUIRE(cp.owner_team == kNoTeam);  // ownership unchanged by a decay
}

TEST_CASE("match: the owner standing on its own point does not re-capture",
          "[match]") {
    ControlPoint cp{};
    control_point_init(cp);

    // Team 0 captures the point.
    const std::array<u32, 2> team0{1u, 0u};
    tick_control_point(cp, counts(team0), 1.0f, 0.5f, 1.0f);  // flips to team 0
    REQUIRE(cp.owner_team == 0u);
    REQUIRE(cp.progress == Catch::Approx(0.0f));

    // The owner keeps standing on it: progress stays at 0 (no re-capture).
    tick_control_point(cp, counts(team0), 1.0f, 0.5f, 1.0f);
    REQUIRE(cp.owner_team == 0u);
    REQUIRE(cp.progress == Catch::Approx(0.0f));
    REQUIRE(cp.capturing_team == kNoTeam);

    // Even with prior partial decay-able progress, the owner can't grow it.
    cp.progress = 0.3f;  // pretend a leftover sliver
    tick_control_point(cp, counts(team0), 1.0f, 0.5f, 1.0f);  // decays -> 0.0
    REQUIRE(cp.progress == Catch::Approx(0.0f));
}

TEST_CASE("match: decay clamps progress at zero (no negative progress)",
          "[match]") {
    ControlPoint cp{};
    control_point_init(cp);
    cp.capturing_team = 1u;
    cp.progress = 0.1f;

    // Decay step larger than the remaining progress must clamp at 0, not go neg.
    const std::array<u32, 3> empty{0u, 0u, 0u};
    tick_control_point(cp, counts(empty), 0.5f, 5.0f, 1.0f);
    REQUIRE(cp.progress == Catch::Approx(0.0f));
    REQUIRE(cp.progress >= 0.0f);
    REQUIRE(cp.capturing_team == kNoTeam);
}

TEST_CASE("match: control-point ticks are bit-deterministic", "[match]") {
    const std::array<u32, 3> team1{0u, 1u, 0u};
    const std::array<u32, 3> contest{1u, 1u, 0u};

    auto run = [&]() {
        ControlPoint cp{};
        control_point_init(cp);
        tick_control_point(cp, counts(team1), 0.3f, 0.15f, 0.5f);
        tick_control_point(cp, counts(team1), 0.3f, 0.15f, 0.5f);
        tick_control_point(cp, counts(contest), 0.3f, 0.15f, 0.5f);
        tick_control_point(cp, counts(team1), 0.3f, 0.15f, 0.5f);
        return cp;
    };

    const ControlPoint a = run();
    const ControlPoint b = run();
    // Identical inputs => identical state, bit for bit.
    REQUIRE(a.owner_team == b.owner_team);
    REQUIRE(a.capturing_team == b.capturing_team);
    REQUIRE(a.progress == b.progress);  // exact equality, not Approx
}
