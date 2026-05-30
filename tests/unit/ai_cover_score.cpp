// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/ai_cover_score.cpp — deterministic tactical cover-position scoring:
// being farther from the nearest threat scores higher and being farther from the
// anchor scores lower; best_cover picks the spot that is safe yet near the
// anchor; a candidate sitting next to a threat loses to a far one; the no-threats
// case reduces to pull-to-anchor; equal scores break to the lower index; empty
// candidates returns false untouched; and identical inputs yield an identical
// pick.

#include "ai/CoverScore.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace psynder;
using namespace psynder::ai;

namespace {
math::Vec3 v(f32 x, f32 z) { return math::Vec3{x, 0.0f, z}; }
}  // namespace

TEST_CASE("cover_score rewards distance from the nearest threat", "[ai][cover]") {
    const std::vector<math::Vec3> threats = {v(0.0f, 0.0f)};
    const math::Vec3 anchor = v(0.0f, 0.0f);
    // Zero anchor weight isolates the threat term: only nearest-threat distance
    // moves the score.
    const CoverWeights w{/*threat=*/1.0f, /*anchor=*/0.0f, /*min=*/0.0f};

    const f32 near_s = cover_score(v(3.0f, 0.0f), threats, anchor, w);
    const f32 far_s = cover_score(v(10.0f, 0.0f), threats, anchor, w);
    REQUIRE(far_s > near_s);
    // With these weights the score IS the nearest-threat distance.
    REQUIRE(near_s == Catch::Approx(3.0f));
    REQUIRE(far_s == Catch::Approx(10.0f));
}

TEST_CASE("cover_score penalizes distance from the anchor", "[ai][cover]") {
    // No threats so the threat term is the same big constant for both; only the
    // anchor penalty differs.
    const std::span<const math::Vec3> no_threats{};
    const math::Vec3 anchor = v(0.0f, 0.0f);
    const CoverWeights w{/*threat=*/1.0f, /*anchor=*/1.0f, /*min=*/2.0f};

    const f32 near_anchor = cover_score(v(2.0f, 0.0f), no_threats, anchor, w);
    const f32 far_anchor = cover_score(v(9.0f, 0.0f), no_threats, anchor, w);
    REQUIRE(near_anchor > far_anchor);  // closer to the anchor is better
    // The difference is exactly the extra anchor distance (7 m) times the weight.
    REQUIRE((near_anchor - far_anchor) == Catch::Approx(7.0f));
}

TEST_CASE("cover_score: a candidate next to a threat scores worse than a far one",
          "[ai][cover]") {
    const std::vector<math::Vec3> threats = {v(0.0f, 0.0f)};
    const math::Vec3 anchor = v(5.0f, 0.0f);  // a shared, neutral objective
    const CoverWeights w = kDefaultCoverWeights;

    const f32 hugging = cover_score(v(0.2f, 0.0f), threats, anchor, w);  // on the threat
    const f32 distant = cover_score(v(12.0f, 0.0f), threats, anchor, w);  // safe
    REQUIRE(distant > hugging);
}

TEST_CASE("cover_score: the min_threat_dist floor caps how much a near threat hurts",
          "[ai][cover]") {
    const std::vector<math::Vec3> threats = {v(0.0f, 0.0f)};
    const math::Vec3 anchor = v(0.0f, 0.0f);
    const CoverWeights w{/*threat=*/1.0f, /*anchor=*/0.0f, /*min=*/2.0f};

    // Two candidates both INSIDE the floor radius (0.5 m and 1.0 m from the
    // threat) get the SAME floored reward of 2.0 — the floor stops the reward
    // collapsing toward 0 near a threat.
    const f32 a = cover_score(v(0.5f, 0.0f), threats, anchor, w);
    const f32 b = cover_score(v(1.0f, 0.0f), threats, anchor, w);
    REQUIRE(a == Catch::Approx(2.0f));
    REQUIRE(b == Catch::Approx(2.0f));
    // A candidate OUTSIDE the floor (5 m) is rewarded by its true distance.
    const f32 c = cover_score(v(5.0f, 0.0f), threats, anchor, w);
    REQUIRE(c == Catch::Approx(5.0f));
    REQUIRE(c > a);
}

TEST_CASE("cover_score: the no-threats case is a large safe reward", "[ai][cover]") {
    const std::span<const math::Vec3> no_threats{};
    const math::Vec3 anchor = v(0.0f, 0.0f);
    const CoverWeights w{/*threat=*/1.0f, /*anchor=*/1.0f, /*min=*/2.0f};

    // score == threat_weight * kNoThreatReward - anchor_weight * d_anchor.
    // At the anchor d_anchor == 0 so the score is exactly the safe constant.
    const f32 at_anchor = cover_score(v(0.0f, 0.0f), no_threats, anchor, w);
    REQUIRE(at_anchor == Catch::Approx(kNoThreatReward));
    // 4 m off the anchor drops the score by exactly 4 (anchor_weight == 1).
    const f32 off = cover_score(v(4.0f, 0.0f), no_threats, anchor, w);
    REQUIRE((at_anchor - off) == Catch::Approx(4.0f));
}

TEST_CASE("best_cover picks a spot away from threats but near the anchor",
          "[ai][cover]") {
    // One threat at the origin, anchor 10 m down +X. The winner should be the
    // candidate that is both clear of the threat AND close to the anchor.
    const std::vector<math::Vec3> threats = {v(0.0f, 0.0f)};
    const math::Vec3 anchor = v(10.0f, 0.0f);
    // Weight anchor proximity ABOVE raw safety here (anchor_weight > threat_weight)
    // so the pick balances cover with staying near the objective rather than just
    // fleeing as far from the threat as possible (which the safety-dominant
    // kDefaultCoverWeights would do — candidate 2 is safest but over-extended).
    const CoverWeights w{1.0f, 2.0f, 2.0f};

    const std::vector<math::Vec3> candidates = {
        v(0.5f, 0.0f),   // 0: hugging the threat, far from anchor — terrible
        v(10.0f, 0.0f),  // 1: 10 m from threat, ON the anchor — clear winner
        v(20.0f, 0.0f),  // 2: very safe but 10 m past the anchor — over-extended
        v(3.0f, 0.0f),   // 3: close-ish to both, mediocre
    };

    usize pick = 999;
    REQUIRE(best_cover(candidates, threats, anchor, w, pick));
    REQUIRE(pick == 1u);
}

TEST_CASE("best_cover: an exact tie breaks to the lower index", "[ai][cover]") {
    // Two candidates symmetric about a single threat and a centred anchor have
    // identical scores; the tie must resolve to the lower index.
    const std::vector<math::Vec3> threats = {v(0.0f, 0.0f)};
    const math::Vec3 anchor = v(0.0f, 0.0f);
    const CoverWeights w = kDefaultCoverWeights;

    const std::vector<math::Vec3> candidates = {
        v(4.0f, 0.0f),   // 0: 4 m from threat, 4 m from anchor
        v(-4.0f, 0.0f),  // 1: mirror image — identical score
    };

    usize pick = 999;
    REQUIRE(best_cover(candidates, threats, anchor, w, pick));
    REQUIRE(pick == 0u);  // lowest index of the tie
}

TEST_CASE("best_cover: empty candidates returns false and leaves out untouched",
          "[ai][cover]") {
    const std::span<const math::Vec3> no_candidates{};
    const std::vector<math::Vec3> threats = {v(1.0f, 1.0f)};
    const math::Vec3 anchor = v(0.0f, 0.0f);

    usize out = 0xABCDu;
    REQUIRE_FALSE(best_cover(no_candidates, threats, anchor, kDefaultCoverWeights, out));
    REQUIRE(out == 0xABCDu);  // untouched
}

TEST_CASE("best_cover is deterministic across identical inputs",
          "[ai][cover][determinism]") {
    const std::vector<math::Vec3> threats = {v(0.0f, 0.0f), v(8.0f, 8.0f)};
    const math::Vec3 anchor = v(4.0f, 4.0f);
    const std::vector<math::Vec3> candidates = {
        v(1.0f, 1.0f), v(5.0f, 2.0f), v(3.0f, 7.0f), v(6.0f, 6.0f), v(2.0f, 5.0f),
    };
    const CoverWeights w = kDefaultCoverWeights;

    usize first = 999;
    usize second = 999;
    REQUIRE(best_cover(candidates, threats, anchor, w, first));
    REQUIRE(best_cover(candidates, threats, anchor, w, second));
    REQUIRE(first == second);

    // And the per-candidate scores are bit-stable run to run.
    for (usize i = 0; i < candidates.size(); ++i) {
        const f32 s1 = cover_score(candidates[i], threats, anchor, w);
        const f32 s2 = cover_score(candidates[i], threats, anchor, w);
        REQUIRE(s1 == s2);
    }
}
