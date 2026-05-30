// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/match_round_timer.cpp — the deterministic time-driven round clock
// (engine/match/RoundTimer): Warmup -> Active -> Overtime/Ended -> Ended phase
// transitions on elapsed time, the optional sudden-death overtime branch,
// phase-remaining countdown, the overshoot remainder carrying across a
// transition (including a single dt that sweeps multiple boundaries), the
// non-finite / non-positive dt guard, and bit-for-bit determinism.

#include "match/RoundTimer.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <limits>

using namespace psynder;
using namespace psynder::match;

namespace {

// A representative config: 3s warmup, 10s round, 5s overtime.
constexpr RoundConfig kCfg{3.0f, 10.0f, 5.0f};

// A config with overtime disabled.
constexpr RoundConfig kNoOt{3.0f, 10.0f, 0.0f};

}  // namespace

TEST_CASE("match: round_init starts in Warmup with zero elapsed", "[match]") {
    RoundTimer t{};
    round_init(t);
    REQUIRE(round_phase(t) == RoundPhase::Warmup);
    REQUIRE(t.elapsed_s == Catch::Approx(0.0f));
    REQUIRE_FALSE(round_ended(t));
}

TEST_CASE("match: ticking within Warmup stays in Warmup", "[match]") {
    RoundTimer t{};
    round_init(t);
    round_tick(t, kCfg, true, 1.0f);
    REQUIRE(round_phase(t) == RoundPhase::Warmup);
    REQUIRE(t.elapsed_s == Catch::Approx(1.0f));
}

TEST_CASE("match: ticking past warmup_s enters Active", "[match]") {
    RoundTimer t{};
    round_init(t);
    round_tick(t, kCfg, true, 2.0f);
    round_tick(t, kCfg, true, 2.0f);  // crosses warmup_s = 3
    REQUIRE(round_phase(t) == RoundPhase::Active);
    // 4 total elapsed - 3 warmup = 1 carried into Active.
    REQUIRE(t.elapsed_s == Catch::Approx(1.0f));
}

TEST_CASE("match: Active times out to Ended with no overtime", "[match]") {
    RoundTimer t{};
    round_init(t);
    round_tick(t, kNoOt, true, 3.0f);   // -> Active, elapsed 0
    REQUIRE(round_phase(t) == RoundPhase::Active);
    round_tick(t, kNoOt, true, 10.0f);  // crosses round_s = 10; no overtime
    REQUIRE(round_phase(t) == RoundPhase::Ended);
    REQUIRE(round_ended(t));
}

TEST_CASE("match: Active times out to Ended when sudden death is resolved",
          "[match]") {
    RoundTimer t{};
    round_init(t);
    round_tick(t, kCfg, true, 3.0f);    // -> Active
    REQUIRE(round_phase(t) == RoundPhase::Active);
    // Overtime IS configured, but the round is resolved => go straight to Ended.
    round_tick(t, kCfg, /*sudden_death_unresolved=*/false, 10.0f);
    REQUIRE(round_phase(t) == RoundPhase::Ended);
}

TEST_CASE("match: Active enters Overtime when configured and unresolved",
          "[match]") {
    RoundTimer t{};
    round_init(t);
    round_tick(t, kCfg, true, 3.0f);   // -> Active
    round_tick(t, kCfg, true, 12.0f);  // crosses round_s = 10, unresolved
    REQUIRE(round_phase(t) == RoundPhase::Overtime);
    // 12 - 10 = 2 carried into Overtime.
    REQUIRE(t.elapsed_s == Catch::Approx(2.0f));

    // Overtime then times out to Ended after overtime_s = 5.
    round_tick(t, kCfg, true, 5.0f);   // 2 + 5 = 7 >= 5
    REQUIRE(round_phase(t) == RoundPhase::Ended);
    REQUIRE(round_ended(t));
}

TEST_CASE("match: phase_remaining counts down and is zero at Ended", "[match]") {
    RoundTimer t{};
    round_init(t);
    REQUIRE(phase_remaining_s(t, kCfg) == Catch::Approx(3.0f));  // full warmup
    round_tick(t, kCfg, true, 1.0f);
    REQUIRE(phase_remaining_s(t, kCfg) == Catch::Approx(2.0f));

    round_tick(t, kCfg, true, 2.0f);  // -> Active (full round remains)
    REQUIRE(round_phase(t) == RoundPhase::Active);
    REQUIRE(phase_remaining_s(t, kCfg) == Catch::Approx(10.0f));

    // Drive to Ended; remaining is 0 there.
    round_tick(t, kCfg, /*resolved=*/false, 10.0f);
    REQUIRE(round_phase(t) == RoundPhase::Ended);
    REQUIRE(phase_remaining_s(t, kCfg) == Catch::Approx(0.0f));
}

TEST_CASE("match: the overshoot remainder carries across a transition",
          "[match]") {
    RoundTimer t{};
    round_init(t);
    // One big dt that overshoots the warmup boundary by 5.5 (8.5 - 3.0).
    round_tick(t, kCfg, true, 8.5f);
    REQUIRE(round_phase(t) == RoundPhase::Active);
    REQUIRE(t.elapsed_s == Catch::Approx(5.5f));
}

TEST_CASE("match: a single dt can sweep multiple phase boundaries", "[match]") {
    RoundTimer t{};
    round_init(t);
    // 3 (warmup) + 10 (round) + 5 (overtime) = 18 to reach Ended; +2 overshoot.
    // Ended parks elapsed at 0 (no duration to carry into).
    round_tick(t, kCfg, /*sudden_death_unresolved=*/true, 20.0f);
    REQUIRE(round_phase(t) == RoundPhase::Ended);
    REQUIRE(t.elapsed_s == Catch::Approx(0.0f));
}

TEST_CASE("match: a single dt can sweep Warmup straight to Ended without "
          "overtime",
          "[match]") {
    RoundTimer t{};
    round_init(t);
    // 3 (warmup) + 10 (round) = 13 to Ended with overtime off.
    round_tick(t, kNoOt, true, 100.0f);
    REQUIRE(round_phase(t) == RoundPhase::Ended);
    REQUIRE(t.elapsed_s == Catch::Approx(0.0f));
}

TEST_CASE("match: non-finite and non-positive dt are ignored", "[match]") {
    RoundTimer t{};
    round_init(t);
    round_tick(t, kCfg, true, 1.0f);
    const f32 before = t.elapsed_s;

    round_tick(t, kCfg, true, 0.0f);                              // zero
    REQUIRE(t.elapsed_s == Catch::Approx(before));
    round_tick(t, kCfg, true, -5.0f);                            // negative
    REQUIRE(t.elapsed_s == Catch::Approx(before));
    round_tick(t, kCfg, true, std::numeric_limits<f32>::quiet_NaN());  // NaN
    REQUIRE(t.elapsed_s == Catch::Approx(before));
    round_tick(t, kCfg, true, std::numeric_limits<f32>::infinity());   // inf
    REQUIRE(t.elapsed_s == Catch::Approx(before));

    REQUIRE(round_phase(t) == RoundPhase::Warmup);
}

TEST_CASE("match: ticking an already-Ended round is a no-op", "[match]") {
    RoundTimer t{};
    round_init(t);
    round_tick(t, kNoOt, false, 100.0f);  // -> Ended
    REQUIRE(round_ended(t));
    const f32 elapsed = t.elapsed_s;
    round_tick(t, kNoOt, false, 50.0f);   // no further advance
    REQUIRE(round_ended(t));
    REQUIRE(t.elapsed_s == Catch::Approx(elapsed));
}

TEST_CASE("match: zero-duration warmup falls through on the first tick",
          "[match]") {
    // warmup_s = 0 => Warmup times out immediately, carrying the full dt.
    constexpr RoundConfig cfg{0.0f, 10.0f, 0.0f};
    RoundTimer t{};
    round_init(t);
    round_tick(t, cfg, true, 2.0f);
    REQUIRE(round_phase(t) == RoundPhase::Active);
    REQUIRE(t.elapsed_s == Catch::Approx(2.0f));  // full dt carried into Active
}

TEST_CASE("match: the round clock is bit-deterministic across replays",
          "[match]") {
    const f32 dts[] = {0.016f, 1.5f, 2.0f, 7.7f, 4.0f, 3.3f, 1.0f, 9.0f};

    auto replay = [&](RoundTimer& t) {
        round_init(t);
        for (const f32 dt : dts) round_tick(t, kCfg, true, dt);
    };

    RoundTimer a{};
    RoundTimer b{};
    replay(a);
    replay(b);

    // Identical inputs => bit-identical phase + elapsed.
    REQUIRE(a.phase == b.phase);
    REQUIRE(std::bit_cast<u32>(a.elapsed_s) == std::bit_cast<u32>(b.elapsed_s));
}
