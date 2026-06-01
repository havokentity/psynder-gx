// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/ai_flank.cpp — flanking position computation.

#include "ai/Flank.h"

#include "math/Math.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace psynder;
using namespace psynder::ai;

namespace {
// Target at the origin facing +X => forward (1,0,0), right (0,0,1).
constexpr math::Vec3 kTgt{0.0f, 0.0f, 0.0f};
constexpr math::Vec3 kFwdX{1.0f, 0.0f, 0.0f};
constexpr f32 kDist = 5.0f;
}  // namespace

TEST_CASE("flank: positions sit on the correct side of the target", "[ai]") {
    const math::Vec3 r = flank_position(kTgt, kFwdX, FlankSide::Right, kDist);
    const math::Vec3 l = flank_position(kTgt, kFwdX, FlankSide::Left, kDist);
    const math::Vec3 b = flank_position(kTgt, kFwdX, FlankSide::Rear, kDist);
    // Right => +right => +Z; Left => -right => -Z; Rear => -forward => -X.
    CHECK(r.x == Catch::Approx(0.0f));
    CHECK(r.z == Catch::Approx(5.0f));
    CHECK(l.z == Catch::Approx(-5.0f));
    CHECK(b.x == Catch::Approx(-5.0f));
    CHECK(b.z == Catch::Approx(0.0f));
    // All in the target's XZ plane.
    CHECK(r.y == Catch::Approx(0.0f));
    CHECK(b.y == Catch::Approx(0.0f));
}

TEST_CASE("flank: a flank position is outside the target front arc", "[ai]") {
    const f32 front_cos = 0.5f;  // 120 deg total cone
    // Straight ahead is seen.
    CHECK(in_front_arc(kTgt, kFwdX, math::Vec3{10.0f, 0.0f, 0.0f}, front_cos));
    // The flank + rear positions are NOT seen.
    const math::Vec3 side = flank_position(kTgt, kFwdX, FlankSide::Right, kDist);
    const math::Vec3 rear = flank_position(kTgt, kFwdX, FlankSide::Rear, kDist);
    CHECK_FALSE(in_front_arc(kTgt, kFwdX, side, front_cos));
    CHECK_FALSE(in_front_arc(kTgt, kFwdX, rear, front_cos));
}

TEST_CASE("flank: best side is the one nearest the attacker", "[ai]") {
    // Attacker already off the target's +Z (right) side -> Right needs least move.
    CHECK(best_flank_side(math::Vec3{0.0f, 0.0f, 10.0f}, kTgt, kFwdX) == FlankSide::Right);
    // Attacker off the -Z (left) side -> Left.
    CHECK(best_flank_side(math::Vec3{0.0f, 0.0f, -10.0f}, kTgt, kFwdX) == FlankSide::Left);
    // Attacker behind the target (-X) -> Rear.
    CHECK(best_flank_side(math::Vec3{-10.0f, 0.0f, 0.0f}, kTgt, kFwdX) == FlankSide::Rear);
}

TEST_CASE("flank: a degenerate facing falls back without NaN", "[ai]") {
    const math::Vec3 p = flank_position(kTgt, math::Vec3{0.0f, 0.0f, 0.0f},
                                        FlankSide::Rear, kDist);
    // Fallback forward (0,0,-1) => right (1,0,0); Rear = -forward = +Z.
    CHECK(p.z == Catch::Approx(5.0f));
    CHECK(p.x == Catch::Approx(0.0f));
}

TEST_CASE("flank: computation is deterministic", "[ai][determinism]") {
    const math::Vec3 facing{0.3f, 0.0f, -0.7f};
    for (int i = 0; i < 16; ++i) {
        const math::Vec3 a = flank_position({1.0f, 2.0f, 3.0f}, facing,
                                            FlankSide::Right, 4.0f);
        const math::Vec3 b = flank_position({1.0f, 2.0f, 3.0f}, facing,
                                            FlankSide::Right, 4.0f);
        CHECK(a.x == b.x);
        CHECK(a.z == b.z);
    }
}
