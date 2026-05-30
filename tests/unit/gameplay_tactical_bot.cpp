// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/gameplay_tactical_bot.cpp — the tactical FSM decision: fight-or-
// flight priority, the visibility gate, threshold edges, the degenerate-health
// guard, ECS storability, and determinism.

#include "gameplay/TacticalBot.h"

#include "scene/World.h"

#include <catch2/catch_test_macros.hpp>

using namespace psynder;
using namespace psynder::gameplay;

TEST_CASE("tactical: a healthy bot engages a visible enemy", "[gameplay]") {
    CHECK(decide_tactical_state(100.0f, 100.0f, true, 0.5f) == TacticalState::Engage);
    // Just above the threshold + a visible enemy still engages.
    CHECK(decide_tactical_state(51.0f, 100.0f, true, 0.5f) == TacticalState::Engage);
}

TEST_CASE("tactical: a healthy bot with no visible enemy patrols", "[gameplay]") {
    CHECK(decide_tactical_state(100.0f, 100.0f, false, 0.5f) == TacticalState::Patrol);
    CHECK(decide_tactical_state(80.0f, 100.0f, false, 0.5f) == TacticalState::Patrol);
}

TEST_CASE("tactical: a hurt bot retreats even with a visible enemy", "[gameplay]") {
    // Survival first: below the threshold, fall back regardless of the enemy.
    CHECK(decide_tactical_state(40.0f, 100.0f, true, 0.5f) == TacticalState::Retreat);
    CHECK(decide_tactical_state(40.0f, 100.0f, false, 0.5f) == TacticalState::Retreat);
    // Exactly at the threshold retreats (<=).
    CHECK(decide_tactical_state(50.0f, 100.0f, true, 0.5f) == TacticalState::Retreat);
}

TEST_CASE("tactical: retreat fraction is clamped and the health pool is guarded",
          "[gameplay]") {
    // frac clamped to [0,1]: a >1 fraction behaves as 1 (retreat at any damage).
    CHECK(decide_tactical_state(99.0f, 100.0f, false, 5.0f) == TacticalState::Retreat);
    // frac clamped at 0: only a dead/zero-health bot retreats.
    CHECK(decide_tactical_state(1.0f, 100.0f, false, -1.0f) == TacticalState::Patrol);
    CHECK(decide_tactical_state(0.0f, 100.0f, false, -1.0f) == TacticalState::Retreat);
    // Degenerate max_hp never forces Retreat.
    CHECK(decide_tactical_state(0.0f, 0.0f, true, 0.5f) == TacticalState::Engage);
    CHECK(decide_tactical_state(0.0f, 0.0f, false, 0.5f) == TacticalState::Patrol);
}

TEST_CASE("tactical: TacticalBot is a storable POD component", "[gameplay]") {
    scene::World w;
    const Entity e = w.create();
    w.add(e, TacticalBot{static_cast<u32>(TacticalState::Patrol), 0.5f});
    TacticalBot* tb = w.get<TacticalBot>(e);
    REQUIRE(tb != nullptr);
    CHECK(tb->state == static_cast<u32>(TacticalState::Patrol));
    tb->state = static_cast<u32>(TacticalState::Engage);
    CHECK(w.get<TacticalBot>(e)->state == static_cast<u32>(TacticalState::Engage));
}

TEST_CASE("tactical: the decision is deterministic", "[gameplay][determinism]") {
    for (int i = 0; i < 64; ++i) {
        const f32 hp = static_cast<f32>(i);
        const bool vis = (i % 2) == 0;
        CHECK(decide_tactical_state(hp, 100.0f, vis, 0.5f) ==
              decide_tactical_state(hp, 100.0f, vis, 0.5f));
    }
}
