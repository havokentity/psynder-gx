// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/ai_formation.cpp — deterministic squad formation slots.

#include "ai/Formation.h"

#include "math/Math.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace psynder;
using namespace psynder::ai;

namespace {
constexpr math::Vec3 kFwdZ{0.0f, 0.0f, -1.0f};  // canonical look (-Z)
constexpr math::Vec3 kFwdX{1.0f, 0.0f, 0.0f};   // looking down +X
constexpr f32 kSp = 2.0f;
}  // namespace

TEST_CASE("formation: slot 0 is the leader for every shape", "[ai]") {
    const math::Vec3 pos{5.0f, 1.0f, -3.0f};
    for (FormationShape s : {FormationShape::Line, FormationShape::Column,
                             FormationShape::Wedge}) {
        const math::Vec3 slot = formation_slot(pos, kFwdZ, s, 0, kSp);
        CHECK(slot.x == Catch::Approx(pos.x));
        CHECK(slot.y == Catch::Approx(pos.y));
        CHECK(slot.z == Catch::Approx(pos.z));
    }
}

TEST_CASE("formation: column trails straight behind the leader", "[ai]") {
    const math::Vec3 pos{0.0f, 0.0f, 0.0f};
    // forward (0,0,-1) => behind is +Z. Member i sits i*spacing back.
    for (u32 i = 1; i <= 3; ++i) {
        const math::Vec3 s = formation_slot(pos, kFwdZ, FormationShape::Column, i, kSp);
        CHECK(s.x == Catch::Approx(0.0f));
        CHECK(s.y == Catch::Approx(0.0f));
        CHECK(s.z == Catch::Approx(static_cast<f32>(i) * kSp));
    }
}

TEST_CASE("formation: line alternates right then left along the right axis",
          "[ai]") {
    const math::Vec3 pos{0.0f, 0.0f, 0.0f};
    // forward (0,0,-1) => right is +X. 1->+1, 2->-1, 3->+2, 4->-2 (x spacing).
    const f32 x1 = formation_slot(pos, kFwdZ, FormationShape::Line, 1, kSp).x;
    const f32 x2 = formation_slot(pos, kFwdZ, FormationShape::Line, 2, kSp).x;
    const f32 x3 = formation_slot(pos, kFwdZ, FormationShape::Line, 3, kSp).x;
    const f32 x4 = formation_slot(pos, kFwdZ, FormationShape::Line, 4, kSp).x;
    CHECK(x1 == Catch::Approx(kSp));
    CHECK(x2 == Catch::Approx(-kSp));
    CHECK(x3 == Catch::Approx(2.0f * kSp));
    CHECK(x4 == Catch::Approx(-2.0f * kSp));
    // Line stays on the leader's lateral axis (no forward/back component).
    CHECK(formation_slot(pos, kFwdZ, FormationShape::Line, 1, kSp).z == Catch::Approx(0.0f));
}

TEST_CASE("formation: wedge fans out behind on both sides", "[ai]") {
    const math::Vec3 pos{0.0f, 0.0f, 0.0f};
    // forward (0,0,-1): behind = +Z, right = +X. Member 1 = back+right,
    // member 2 = back+left, both one step (z grows, |x| grows).
    const math::Vec3 m1 = formation_slot(pos, kFwdZ, FormationShape::Wedge, 1, kSp);
    const math::Vec3 m2 = formation_slot(pos, kFwdZ, FormationShape::Wedge, 2, kSp);
    CHECK(m1.z == Catch::Approx(kSp));
    CHECK(m1.x == Catch::Approx(kSp));
    CHECK(m2.z == Catch::Approx(kSp));
    CHECK(m2.x == Catch::Approx(-kSp));
    // Member 3 steps further back and out.
    const math::Vec3 m3 = formation_slot(pos, kFwdZ, FormationShape::Wedge, 3, kSp);
    CHECK(m3.z == Catch::Approx(2.0f * kSp));
    CHECK(m3.x == Catch::Approx(2.0f * kSp));
}

TEST_CASE("formation: a rotated forward rotates the slots", "[ai]") {
    const math::Vec3 pos{0.0f, 0.0f, 0.0f};
    // forward (1,0,0): right = (0,0,1) (+Z), behind = -X.
    // Column member 1 sits one step behind => -X.
    const math::Vec3 col = formation_slot(pos, kFwdX, FormationShape::Column, 1, kSp);
    CHECK(col.x == Catch::Approx(-kSp));
    CHECK(col.z == Catch::Approx(0.0f));
    // Line member 1 sits +1 right => +Z.
    const math::Vec3 line = formation_slot(pos, kFwdX, FormationShape::Line, 1, kSp);
    CHECK(line.z == Catch::Approx(kSp));
    CHECK(line.x == Catch::Approx(0.0f));
}

TEST_CASE("formation: a zero forward falls back without NaN", "[ai]") {
    const math::Vec3 pos{0.0f, 0.0f, 0.0f};
    const math::Vec3 s = formation_slot(pos, {0.0f, 0.0f, 0.0f},
                                        FormationShape::Column, 1, kSp);
    // Fallback forward (0,0,-1): one step behind => +Z, finite.
    CHECK(s.z == Catch::Approx(kSp));
    CHECK(s.x == Catch::Approx(0.0f));
}

TEST_CASE("formation: formation_slots fills count entries", "[ai]") {
    std::vector<math::Vec3> out;
    formation_slots({0.0f, 0.0f, 0.0f}, kFwdZ, FormationShape::Wedge, 5, kSp, out);
    REQUIRE(out.size() == 5u);
    for (u32 i = 0; i < 5; ++i) {
        const math::Vec3 s = formation_slot({0.0f, 0.0f, 0.0f}, kFwdZ,
                                            FormationShape::Wedge, i, kSp);
        CHECK(out[i].x == Catch::Approx(s.x));
        CHECK(out[i].z == Catch::Approx(s.z));
    }
}

TEST_CASE("formation: slots are deterministic", "[ai][determinism]") {
    for (u32 i = 0; i < 8; ++i) {
        const math::Vec3 a = formation_slot({1.0f, 2.0f, 3.0f}, kFwdX,
                                            FormationShape::Wedge, i, kSp);
        const math::Vec3 b = formation_slot({1.0f, 2.0f, 3.0f}, kFwdX,
                                            FormationShape::Wedge, i, kSp);
        CHECK(a.x == b.x);
        CHECK(a.y == b.y);
        CHECK(a.z == b.z);
    }
}
