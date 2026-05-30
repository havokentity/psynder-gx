// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/gameplay_heal.cpp — deterministic healing: instant medkit + HoT.
// Covers: apply_heal tops off at the cap and reports the amount restored; healing
// at the cap restores nothing; overheal past max works; a dead entity is not
// revived; grant_hot + tick_heals heals over time then expires (the buff is
// removed); and the whole thing is bit-reproducible.

#include "gameplay/GameplayComponents.h"
#include "gameplay/Heal.h"

#include "scene/World.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

using namespace psynder;
using namespace psynder::gameplay;
using psynder::scene::World;

namespace {
Entity spawn_health(World& w, f32 hp, f32 max_hp) {
    const Entity e = w.create();
    w.add(e, Health{hp, max_hp});
    return e;
}
}  // namespace

TEST_CASE("gameplay: apply_heal restores hp capped at over_max_cap and reports it",
          "[gameplay]") {
    World w;
    const Entity e = spawn_health(w, 30.0f, 100.0f);

    // Normal heal (cap == max_hp) below the cap: full amount lands.
    REQUIRE(apply_heal(w, e, 25.0f, 100.0f) == Catch::Approx(25.0f));
    REQUIRE(w.get<Health>(e)->hp == Catch::Approx(55.0f));

    // Heal that would overshoot the cap clamps and reports only what was applied.
    REQUIRE(apply_heal(w, e, 70.0f, 100.0f) == Catch::Approx(45.0f));
    REQUIRE(w.get<Health>(e)->hp == Catch::Approx(100.0f));
}

TEST_CASE("gameplay: healing at or above the cap restores nothing", "[gameplay]") {
    World w;
    const Entity e = spawn_health(w, 100.0f, 100.0f);
    REQUIRE(apply_heal(w, e, 50.0f, 100.0f) == Catch::Approx(0.0f));
    REQUIRE(w.get<Health>(e)->hp == Catch::Approx(100.0f));

    // A non-positive amount is a no-op too.
    REQUIRE(apply_heal(w, e, -10.0f, 100.0f) == Catch::Approx(0.0f));
    REQUIRE(w.get<Health>(e)->hp == Catch::Approx(100.0f));

    // Healing on an entity with no Health component restores nothing.
    const Entity bare = w.create();
    REQUIRE(apply_heal(w, bare, 50.0f, 100.0f) == Catch::Approx(0.0f));
}

TEST_CASE("gameplay: overheal past max works when the cap exceeds max_hp",
          "[gameplay]") {
    World w;
    const Entity e = spawn_health(w, 100.0f, 100.0f);
    // Quake-style overheal: cap of 200 lets hp climb above max_hp.
    REQUIRE(apply_heal(w, e, 150.0f, 200.0f) == Catch::Approx(100.0f));
    REQUIRE(w.get<Health>(e)->hp == Catch::Approx(200.0f));
    // Now at the overheal cap: further healing to the same cap restores nothing.
    REQUIRE(apply_heal(w, e, 25.0f, 200.0f) == Catch::Approx(0.0f));
    REQUIRE(w.get<Health>(e)->hp == Catch::Approx(200.0f));
}

TEST_CASE("gameplay: a dead entity is not revived by a heal", "[gameplay]") {
    World w;
    const Entity e = spawn_health(w, 0.0f, 100.0f);  // hp <= 0 => dead
    REQUIRE(apply_heal(w, e, 50.0f, 100.0f) == Catch::Approx(0.0f));
    REQUIRE(w.get<Health>(e)->hp == Catch::Approx(0.0f));
}

TEST_CASE("gameplay: grant_hot then tick_heals heals over time and expires",
          "[gameplay]") {
    World w;
    const Entity e = spawn_health(w, 50.0f, 100.0f);
    // 10 hp/s for 2 s, capped at max (100).
    grant_hot(w, e, 10.0f, 2.0f, 100.0f);
    REQUIRE(w.get<HealOverTime>(e) != nullptr);

    std::vector<Entity> scratch;
    // 1 s of buff: 10 ticks of 0.1 s each => +10 hp; buff still active (1 s left).
    for (int i = 0; i < 10; ++i) tick_heals(w, 0.1f, scratch);
    REQUIRE(w.get<Health>(e)->hp == Catch::Approx(60.0f));
    REQUIRE(w.get<HealOverTime>(e) != nullptr);

    // Remaining 1 s: another +10 hp; on the tick that drives remaining_s to 0 the
    // buff expires and the component is removed.
    for (int i = 0; i < 10; ++i) tick_heals(w, 0.1f, scratch);
    REQUIRE(w.get<Health>(e)->hp == Catch::Approx(70.0f));
    REQUIRE(w.get<HealOverTime>(e) == nullptr);

    // After expiry, ticking does nothing more.
    tick_heals(w, 0.1f, scratch);
    REQUIRE(w.get<Health>(e)->hp == Catch::Approx(70.0f));
}

TEST_CASE("gameplay: grant_hot refreshes to the stronger buff per field",
          "[gameplay]") {
    World w;
    const Entity e = spawn_health(w, 50.0f, 100.0f);
    grant_hot(w, e, 5.0f, 1.0f, 100.0f);
    // A second grant: stronger rate + cap, but a SHORTER duration than what's left.
    grant_hot(w, e, 20.0f, 0.5f, 200.0f);
    const HealOverTime* hot = w.get<HealOverTime>(e);
    REQUIRE(hot != nullptr);
    REQUIRE(hot->rate_per_s == Catch::Approx(20.0f));   // stronger rate kept
    REQUIRE(hot->remaining_s == Catch::Approx(1.0f));   // longer duration kept
    REQUIRE(hot->over_max_cap == Catch::Approx(200.0f)); // higher cap kept
}

TEST_CASE("gameplay: healing is deterministic across worlds",
          "[gameplay][determinism]") {
    const auto run = []() {
        World w;
        std::vector<Entity> es;
        for (int i = 0; i < 8; ++i) {
            const Entity e = w.create();
            w.add(e, Health{20.0f + static_cast<f32>(i), 100.0f});
            es.push_back(e);
            grant_hot(w, e, 3.0f + static_cast<f32>(i % 3), 1.5f, 150.0f);
        }
        std::vector<Entity> scratch;
        constexpr f32 dt = 1.0f / 120.0f;
        for (int step = 0; step < 240; ++step) {
            // Deterministic instant-heal pattern interleaved with the HoT ticks.
            for (std::size_t i = 0; i < es.size(); ++i) {
                apply_heal(w, es[i], 0.5f + static_cast<f32>((step + i) % 4),
                           150.0f);
            }
            tick_heals(w, dt, scratch);
        }
        std::vector<f32> out;
        for (Entity e : es) {
            out.push_back(w.get<Health>(e)->hp);
            const HealOverTime* hot = w.get<HealOverTime>(e);
            out.push_back(hot ? hot->remaining_s : -1.0f);
        }
        return out;
    };
    const std::vector<f32> a = run();
    const std::vector<f32> b = run();
    REQUIRE(a == b);
}
