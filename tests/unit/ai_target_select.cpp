// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/ai_target_select.cpp — deterministic best-target selection: the
// composite score rewards nearer / visible / lower-health targets, the selector
// ignores teammates and out-of-range candidates, picks the clear winner, lets a
// dominant visibility bonus beat a closer hidden enemy, breaks exact ties to the
// lower id, reports no eligible enemy, and is bit-reproducible.

#include "ai/TargetSelect.h"

#include "math/Math.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace psynder;
using namespace psynder::ai;

namespace {
constexpr math::Vec3 kOrigin{0.0f, 0.0f, 0.0f};

math::Vec3 at_x(f32 x) { return {x, 0.0f, 0.0f}; }
}  // namespace

TEST_CASE("ai: target_score rewards nearer targets", "[ai][target]") {
    const TargetWeights& w = kDefaultTargetWeights;
    const f32 near_s = target_score(kOrigin, at_x(5.0f), 100.0f, true, w);
    const f32 far_s = target_score(kOrigin, at_x(20.0f), 100.0f, true, w);
    REQUIRE(near_s > far_s);
}

TEST_CASE("ai: target_score rewards visible targets", "[ai][target]") {
    const TargetWeights& w = kDefaultTargetWeights;
    const f32 seen = target_score(kOrigin, at_x(10.0f), 100.0f, true, w);
    const f32 hidden = target_score(kOrigin, at_x(10.0f), 100.0f, false, w);
    REQUIRE(seen > hidden);
    REQUIRE((seen - hidden) == Catch::Approx(w.visible_bonus));
}

TEST_CASE("ai: target_score rewards lower-health targets", "[ai][target]") {
    const TargetWeights& w = kDefaultTargetWeights;
    const f32 wounded = target_score(kOrigin, at_x(10.0f), 10.0f, true, w);
    const f32 healthy = target_score(kOrigin, at_x(10.0f), 100.0f, true, w);
    REQUIRE(wounded > healthy);
}

TEST_CASE("ai: target_score formula matches the documented expression",
          "[ai][target]") {
    const TargetWeights& w = kDefaultTargetWeights;
    const f32 s = target_score(kOrigin, at_x(10.0f), 50.0f, true, w);
    // visible_bonus*1 - distance_weight*10 + low_health_bonus*(1 - 0.5)
    const f32 expected = w.visible_bonus * 1.0f - w.distance_weight * 10.0f
                       + w.low_health_bonus * 0.5f;
    REQUIRE(s == Catch::Approx(expected));
}

TEST_CASE("ai: select_target ignores same-team candidates", "[ai][target]") {
    const std::vector<TargetCandidate> cands = {
        // A very attractive teammate (close, visible, near-dead) must be skipped.
        {7u, at_x(1.0f), /*team=*/0u, 1.0f, true},
        {3u, at_x(15.0f), /*team=*/1u, 100.0f, true},  // the only real enemy
    };
    u32 picked = 0xDEADu;
    const bool ok =
        select_target(kOrigin, /*chooser_team=*/0u, cands, kDefaultTargetWeights, picked);
    REQUIRE(ok);
    REQUIRE(picked == 3u);
}

TEST_CASE("ai: select_target ignores candidates beyond max_range_m",
          "[ai][target]") {
    TargetWeights w = kDefaultTargetWeights;
    w.max_range_m = 10.0f;
    const std::vector<TargetCandidate> cands = {
        {1u, at_x(9.5f), 1u, 100.0f, true},   // just inside range
        {2u, at_x(50.0f), 1u, 1.0f, true},    // far more attractive but out of range
    };
    u32 picked = 0u;
    const bool ok = select_target(kOrigin, 0u, cands, w, picked);
    REQUIRE(ok);
    REQUIRE(picked == 1u);
}

TEST_CASE("ai: select_target picks the highest-scoring enemy", "[ai][target]") {
    // Candidate 2 is the clear winner: close, visible, wounded. The others are
    // each worse on a different axis.
    const std::vector<TargetCandidate> cands = {
        {1u, at_x(40.0f), 1u, 100.0f, true},   // far, healthy
        {2u, at_x(5.0f), 1u, 10.0f, true},     // close, wounded, visible  <- best
        {3u, at_x(8.0f), 1u, 100.0f, false},   // close-ish but hidden + healthy
    };
    u32 picked = 0u;
    const bool ok = select_target(kOrigin, 0u, cands, kDefaultTargetWeights, picked);
    REQUIRE(ok);
    REQUIRE(picked == 2u);
}

TEST_CASE("ai: a visible enemy beats a closer hidden one when the bonus dominates",
          "[ai][target]") {
    // Hidden enemy is much closer, but the visible bonus outweighs the few metres
    // of extra distance, so the visible one is chosen.
    const std::vector<TargetCandidate> cands = {
        {1u, at_x(2.0f), 1u, 100.0f, false},   // very close but hidden
        {2u, at_x(12.0f), 1u, 100.0f, true},   // farther but visible  <- best
    };
    u32 picked = 0u;
    const bool ok = select_target(kOrigin, 0u, cands, kDefaultTargetWeights, picked);
    REQUIRE(ok);
    REQUIRE(picked == 2u);
}

TEST_CASE("ai: an exact-score tie breaks to the lower id", "[ai][target]") {
    // Two enemies with identical position / health / visibility => identical
    // scores. The lower id must win regardless of list order.
    const std::vector<TargetCandidate> cands = {
        {9u, at_x(10.0f), 1u, 100.0f, true},
        {4u, at_x(10.0f), 1u, 100.0f, true},  // same score, lower id  <- winner
    };
    u32 picked = 0u;
    const bool ok = select_target(kOrigin, 0u, cands, kDefaultTargetWeights, picked);
    REQUIRE(ok);
    REQUIRE(picked == 4u);

    // Reversed input order must not change the outcome.
    const std::vector<TargetCandidate> rev = {cands[1], cands[0]};
    u32 picked2 = 0u;
    const bool ok2 = select_target(kOrigin, 0u, rev, kDefaultTargetWeights, picked2);
    REQUIRE(ok2);
    REQUIRE(picked2 == 4u);
}

TEST_CASE("ai: select_target returns false when there are no eligible enemies",
          "[ai][target]") {
    SECTION("no candidates at all") {
        const std::vector<TargetCandidate> none;
        u32 picked = 0x5151u;
        const bool ok =
            select_target(kOrigin, 0u, none, kDefaultTargetWeights, picked);
        REQUIRE_FALSE(ok);
        REQUIRE(picked == 0x5151u);  // out_id untouched
    }
    SECTION("all teammates or all out of range") {
        TargetWeights w = kDefaultTargetWeights;
        w.max_range_m = 10.0f;
        const std::vector<TargetCandidate> cands = {
            {1u, at_x(2.0f), /*team=*/0u, 50.0f, true},   // teammate
            {2u, at_x(99.0f), /*team=*/1u, 50.0f, true},  // enemy but out of range
        };
        u32 picked = 0x7777u;
        const bool ok = select_target(kOrigin, 0u, cands, w, picked);
        REQUIRE_FALSE(ok);
        REQUIRE(picked == 0x7777u);  // out_id untouched
    }
}

TEST_CASE("ai: select_target is deterministic", "[ai][target][determinism]") {
    const std::vector<TargetCandidate> cands = {
        {5u, {3.0f, 0.0f, 4.0f}, 1u, 80.0f, true},
        {2u, {-7.0f, 0.0f, 1.0f}, 2u, 30.0f, false},
        {8u, {12.0f, 0.0f, -5.0f}, 1u, 55.0f, true},
        {1u, {1.0f, 0.0f, 1.0f}, 0u, 5.0f, true},  // teammate, must be skipped
    };
    const auto run = [&]() {
        u32 id = 0u;
        const bool ok =
            select_target(kOrigin, 0u, cands, kDefaultTargetWeights, id);
        return std::pair<bool, u32>{ok, id};
    };
    REQUIRE(run() == run());
}
