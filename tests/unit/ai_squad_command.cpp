// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/ai_squad_command.cpp — squad orders and per-member goals.

#include "ai/SquadCommand.h"
#include "ai/Formation.h"

#include "math/Math.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <span>
#include <vector>

using namespace psynder;
using namespace psynder::ai;

namespace {
constexpr math::Vec3 kFwdZ{0.0f, 0.0f, -1.0f};  // canonical look (-Z)
constexpr math::Vec3 kFwdX{1.0f, 0.0f, 0.0f};   // looking down +X
constexpr f32 kSp = 2.0f;
}  // namespace

TEST_CASE("squad: set_order stores the order and rally point", "[ai]") {
    SquadState s{};
    const math::Vec3 rally{10.0f, 1.0f, -4.0f};
    squad_set_order(s, SquadOrder::Advance, rally);
    CHECK(s.order == SquadOrder::Advance);
    CHECK(s.rally_point.x == Catch::Approx(rally.x));
    CHECK(s.rally_point.y == Catch::Approx(rally.y));
    CHECK(s.rally_point.z == Catch::Approx(rally.z));

    squad_set_order(s, SquadOrder::Regroup, math::Vec3{0.0f, 0.0f, 0.0f});
    CHECK(s.order == SquadOrder::Regroup);
}

TEST_CASE("squad: advance goals are wedge slots around the rally point",
          "[ai]") {
    SquadState s{};
    const math::Vec3 rally{20.0f, 0.0f, 5.0f};
    squad_set_order(s, SquadOrder::Advance, rally);

    // Leader is somewhere else entirely; Advance must anchor on the rally, not
    // the leader, facing leader_forward (-Z => behind is +Z, right is +X).
    const math::Vec3 leader{0.0f, 0.0f, 0.0f};

    const math::Vec3 m1 = squad_member_goal(s, leader, kFwdZ, 1, kSp);
    const math::Vec3 m2 = squad_member_goal(s, leader, kFwdZ, 2, kSp);
    // Member 1 = one step back + right of the rally; member 2 = back + left.
    CHECK(m1.x == Catch::Approx(rally.x + kSp));
    CHECK(m1.z == Catch::Approx(rally.z + kSp));
    CHECK(m2.x == Catch::Approx(rally.x - kSp));
    CHECK(m2.z == Catch::Approx(rally.z + kSp));

    // Member 1 trails BEHIND the rally and is not the leader's position.
    CHECK(m1.z > rally.z);
    CHECK_FALSE(m1.x == Catch::Approx(leader.x));

    // It matches Formation directly anchored at the rally.
    const math::Vec3 fs =
        formation_slot(rally, kFwdZ, FormationShape::Wedge, 1, kSp);
    CHECK(m1.x == Catch::Approx(fs.x));
    CHECK(m1.z == Catch::Approx(fs.z));
}

TEST_CASE("squad: hold goals are wedge slots around the leader's position",
          "[ai]") {
    SquadState s{};
    // Stale rally point that Hold must ignore.
    squad_set_order(s, SquadOrder::Hold, math::Vec3{99.0f, 0.0f, 99.0f});

    const math::Vec3 leader{3.0f, 1.0f, -2.0f};
    const math::Vec3 m1 = squad_member_goal(s, leader, kFwdZ, 1, kSp);
    const math::Vec3 fs =
        formation_slot(leader, kFwdZ, FormationShape::Wedge, 1, kSp);
    CHECK(m1.x == Catch::Approx(fs.x));
    CHECK(m1.y == Catch::Approx(fs.y));
    CHECK(m1.z == Catch::Approx(fs.z));
    // Anchored on the leader, far from the stale rally.
    CHECK(m1.x == Catch::Approx(leader.x + kSp));
    CHECK(m1.z == Catch::Approx(leader.z + kSp));
}

TEST_CASE("squad: regroup goals all collapse onto the leader", "[ai]") {
    SquadState s{};
    squad_set_order(s, SquadOrder::Regroup, math::Vec3{50.0f, 0.0f, 50.0f});

    const math::Vec3 leader{-4.0f, 2.0f, 7.0f};
    for (u32 i = 0; i < 5; ++i) {
        const math::Vec3 g = squad_member_goal(s, leader, kFwdX, i, kSp);
        CHECK(g.x == Catch::Approx(leader.x));
        CHECK(g.y == Catch::Approx(leader.y));
        CHECK(g.z == Catch::Approx(leader.z));
    }
}

TEST_CASE("squad: all_arrived is true when members sit on their goals",
          "[ai]") {
    SquadState s{};
    const math::Vec3 leader{1.0f, 0.0f, 1.0f};
    squad_set_order(s, SquadOrder::Hold, math::Vec3{0.0f, 0.0f, 0.0f});

    // Three members; member i (0-based) targets formation slot i+1.
    std::array<math::Vec3, 3> pos{};
    for (u32 i = 0; i < 3; ++i) {
        pos[i] = squad_member_goal(s, leader, kFwdZ, i + 1u, kSp);
    }
    CHECK(squad_all_arrived(
        s, std::span<const math::Vec3>(pos.data(), pos.size()), leader, kFwdZ,
        kSp, 0.01f));
}

TEST_CASE("squad: all_arrived is false when one member is far away", "[ai]") {
    SquadState s{};
    const math::Vec3 leader{1.0f, 0.0f, 1.0f};
    squad_set_order(s, SquadOrder::Hold, math::Vec3{0.0f, 0.0f, 0.0f});

    std::array<math::Vec3, 3> pos{};
    for (u32 i = 0; i < 3; ++i) {
        pos[i] = squad_member_goal(s, leader, kFwdZ, i + 1u, kSp);
    }
    // Drag one member far off its slot in XZ.
    pos[1].x += 100.0f;
    CHECK_FALSE(squad_all_arrived(
        s, std::span<const math::Vec3>(pos.data(), pos.size()), leader, kFwdZ,
        kSp, 0.5f));
}

TEST_CASE("squad: all_arrived ignores the Y axis", "[ai]") {
    SquadState s{};
    const math::Vec3 leader{0.0f, 0.0f, 0.0f};
    squad_set_order(s, SquadOrder::Hold, math::Vec3{0.0f, 0.0f, 0.0f});

    std::array<math::Vec3, 2> pos{};
    for (u32 i = 0; i < 2; ++i) {
        pos[i] = squad_member_goal(s, leader, kFwdZ, i + 1u, kSp);
        pos[i].y += 1000.0f;  // huge vertical offset must not matter (XZ check)
    }
    CHECK(squad_all_arrived(
        s, std::span<const math::Vec3>(pos.data(), pos.size()), leader, kFwdZ,
        kSp, 0.01f));
}

TEST_CASE("squad: empty member list counts as arrived", "[ai]") {
    SquadState s{};
    squad_set_order(s, SquadOrder::Advance, math::Vec3{5.0f, 0.0f, 5.0f});
    std::span<const math::Vec3> none{};
    CHECK(squad_all_arrived(s, none, math::Vec3{0.0f, 0.0f, 0.0f}, kFwdZ, kSp,
                            0.25f));
}

TEST_CASE("squad: goals are deterministic", "[ai][determinism]") {
    SquadState s{};
    squad_set_order(s, SquadOrder::Advance, math::Vec3{7.0f, 1.0f, -3.0f});
    const math::Vec3 leader{2.0f, 0.0f, 4.0f};
    for (u32 i = 0; i < 8; ++i) {
        const math::Vec3 a = squad_member_goal(s, leader, kFwdX, i, kSp);
        const math::Vec3 b = squad_member_goal(s, leader, kFwdX, i, kSp);
        CHECK(a.x == b.x);
        CHECK(a.y == b.y);
        CHECK(a.z == b.z);
    }
}
