// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/gameplay_suppression.cpp — suppression: incoming fire raises level,
// it decays over time, high suppression widens spread, and the whole thing is
// deterministic.

#include "gameplay/GameplayComponents.h"
#include "gameplay/Suppression.h"

#include "scene/World.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <limits>
#include <vector>

using namespace psynder;
using namespace psynder::gameplay;
using psynder::scene::World;

namespace {
// A typical pawn: calm to start, decays at 0.5/s, doubles spread when fully
// suppressed.
Suppression make_suppression() noexcept {
    return Suppression{/*level=*/0.0f, /*decay_per_s=*/0.5f,
                       /*max_spread_mult=*/2.0f};
}
}  // namespace

TEST_CASE("gameplay: add_suppression raises level and clamps at one",
          "[gameplay][suppression]") {
    Suppression s = make_suppression();
    add_suppression(s, 0.3f);
    REQUIRE(s.level == Catch::Approx(0.3f));
    add_suppression(s, 0.4f);
    REQUIRE(s.level == Catch::Approx(0.7f));

    // Piling on past full clamps at 1, never above.
    add_suppression(s, 5.0f);
    REQUIRE(s.level == Catch::Approx(1.0f));
    REQUIRE(s.level <= 1.0f);

    // A negative amount lowers it but cannot go below 0.
    add_suppression(s, -10.0f);
    REQUIRE(s.level == Catch::Approx(0.0f));
    REQUIRE(s.level >= 0.0f);
}

TEST_CASE("gameplay: tick_suppression decays level toward zero and clamps",
          "[gameplay][suppression]") {
    Suppression s = make_suppression();
    s.level = 1.0f;
    // 1 s of decay at 0.5/s -> 1.0 -> 0.5.
    for (int i = 0; i < 100; ++i) tick_suppression(s, 1.0f / 100.0f);
    REQUIRE(s.level == Catch::Approx(0.5f));

    // Decay far past 0: clamps at 0, never negative.
    for (int i = 0; i < 1000; ++i) tick_suppression(s, 1.0f / 100.0f);
    REQUIRE(s.level == Catch::Approx(0.0f));
    REQUIRE(s.level >= 0.0f);
}

TEST_CASE("gameplay: spread_multiplier is one at zero, max at one, mid in between",
          "[gameplay][suppression]") {
    Suppression s = make_suppression();  // max_spread_mult = 2.0

    s.level = 0.0f;
    REQUIRE(spread_multiplier(s) == Catch::Approx(1.0f));  // no penalty when calm

    s.level = 1.0f;
    REQUIRE(spread_multiplier(s) == Catch::Approx(2.0f));  // full penalty

    s.level = 0.5f;
    REQUIRE(spread_multiplier(s) == Catch::Approx(1.5f));  // halfway lerp

    // A wider penalty curve lerps the same way.
    s.max_spread_mult = 3.0f;
    s.level = 0.25f;
    REQUIRE(spread_multiplier(s) == Catch::Approx(1.5f));  // 1 + 2*0.25
}

TEST_CASE("gameplay: is_suppressed gates on the threshold",
          "[gameplay][suppression]") {
    Suppression s = make_suppression();
    s.level = 0.0f;
    REQUIRE_FALSE(is_suppressed(s, 0.5f));
    s.level = 0.5f;
    REQUIRE(is_suppressed(s, 0.5f));  // >= is inclusive
    s.level = 0.49f;
    REQUIRE_FALSE(is_suppressed(s, 0.5f));
    s.level = 0.8f;
    REQUIRE(is_suppressed(s, 0.5f));
}

TEST_CASE("gameplay: tick_suppressions decays an ECS Suppression",
          "[gameplay][suppression]") {
    World w;
    const Entity e = w.create();
    Suppression s = make_suppression();
    s.level = 0.8f;
    w.add(e, s);

    // 1 s of world decay at 0.5/s -> 0.8 -> 0.3.
    for (int i = 0; i < 100; ++i) tick_suppressions(w, 1.0f / 100.0f);
    REQUIRE(w.get<Suppression>(e)->level == Catch::Approx(0.3f));
}

TEST_CASE("gameplay: tick_suppression guards a zero or non-finite dt",
          "[gameplay][suppression]") {
    Suppression s = make_suppression();
    s.level = 0.6f;

    // Zero dt is a no-op.
    tick_suppression(s, 0.0f);
    REQUIRE(s.level == Catch::Approx(0.6f));

    // Negative dt is a no-op (never RAISES level by decaying "backwards").
    tick_suppression(s, -1.0f);
    REQUIRE(s.level == Catch::Approx(0.6f));

    // A NaN / infinite dt cannot corrupt the level.
    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    const f32 inf = std::numeric_limits<f32>::infinity();
    tick_suppression(s, nan);
    REQUIRE(s.level == Catch::Approx(0.6f));
    tick_suppression(s, inf);
    REQUIRE(s.level == Catch::Approx(0.6f));
}

TEST_CASE("gameplay: suppression raise + decay is deterministic across runs",
          "[gameplay][determinism]") {
    const auto run = []() {
        Suppression s = make_suppression();
        constexpr f32 dt = 1.0f / 120.0f;
        // Take fire in bursts, coast between — repeated.
        for (int rep = 0; rep < 3; ++rep) {
            add_suppression(s, 0.35f);
            for (int i = 0; i < 60; ++i) tick_suppression(s, dt);
            add_suppression(s, 0.5f);
            for (int i = 0; i < 240; ++i) tick_suppression(s, dt);
        }
        return std::vector<f32>{s.level, spread_multiplier(s)};
    };
    REQUIRE(run() == run());
}
