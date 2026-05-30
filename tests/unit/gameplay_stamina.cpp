// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/gameplay_stamina.cpp — sprint stamina: drain while sprinting,
// regen-after-delay once stopped, fraction readout, and determinism.

#include "gameplay/GameplayComponents.h"
#include "gameplay/Stamina.h"

#include "scene/World.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <vector>

using namespace psynder;
using namespace psynder::gameplay;
using psynder::scene::World;

namespace {
// A typical sprint resource: 100 cap, drains 25/s, regens 20/s after a 1 s wait.
Stamina make_stamina() noexcept {
    return Stamina{/*current=*/100.0f, /*max_stamina=*/100.0f,
                   /*drain_per_s=*/25.0f, /*regen_per_s=*/20.0f,
                   /*regen_delay_s=*/1.0f, /*regen_cooldown_s=*/0.0f};
}
}  // namespace

TEST_CASE("gameplay: sprinting drains stamina and clamps at zero",
          "[gameplay][stamina]") {
    Stamina s = make_stamina();
    // 1 s of sprinting at 25/s drains 25 points.
    for (int i = 0; i < 100; ++i) tick_stamina(s, /*sprinting=*/true, 1.0f / 100.0f);
    REQUIRE(s.current == Catch::Approx(75.0f));

    // Sprint long past empty: clamps at 0, never negative.
    for (int i = 0; i < 1000; ++i) tick_stamina(s, /*sprinting=*/true, 1.0f / 100.0f);
    REQUIRE(s.current == Catch::Approx(0.0f));
    REQUIRE(s.current >= 0.0f);
}

TEST_CASE("gameplay: can_sprint is false once stamina is empty",
          "[gameplay][stamina]") {
    Stamina s = make_stamina();
    REQUIRE(can_sprint(s));
    s.current = 0.0f;
    REQUIRE_FALSE(can_sprint(s));
    s.current = 0.01f;
    REQUIRE(can_sprint(s));
}

TEST_CASE("gameplay: stopping starts the regen delay and does not regen during it",
          "[gameplay][stamina]") {
    Stamina s = make_stamina();
    // Burn some stamina (1 s sprint -> 75 left), which also arms the regen wait.
    for (int i = 0; i < 100; ++i) tick_stamina(s, /*sprinting=*/true, 1.0f / 100.0f);
    REQUIRE(s.current == Catch::Approx(75.0f));
    REQUIRE(s.regen_cooldown_s == Catch::Approx(1.0f));  // = regen_delay_s

    // Stop sprinting: for the first 0.5 s (< 1 s delay) there is NO regen.
    for (int i = 0; i < 50; ++i) tick_stamina(s, /*sprinting=*/false, 1.0f / 100.0f);
    REQUIRE(s.current == Catch::Approx(75.0f));
    REQUIRE(s.regen_cooldown_s == Catch::Approx(0.5f));  // counting down
}

TEST_CASE("gameplay: after the delay regen refills toward max and clamps at max",
          "[gameplay][stamina]") {
    Stamina s = make_stamina();
    s.current = 50.0f;
    s.regen_cooldown_s = 0.0f;  // delay already elapsed

    // 1 s of regen at 20/s -> +20 -> 70.
    for (int i = 0; i < 100; ++i) tick_stamina(s, /*sprinting=*/false, 1.0f / 100.0f);
    REQUIRE(s.current == Catch::Approx(70.0f));

    // Regen far past full: clamps at max_stamina.
    for (int i = 0; i < 1000; ++i) tick_stamina(s, /*sprinting=*/false, 1.0f / 100.0f);
    REQUIRE(s.current == Catch::Approx(100.0f));
    REQUIRE(s.current <= s.max_stamina);
}

TEST_CASE("gameplay: sprinting resets the regen cooldown, interrupting regen",
          "[gameplay][stamina]") {
    Stamina s = make_stamina();
    s.current = 50.0f;
    s.regen_cooldown_s = 0.2f;  // mid-wait, about to start regen

    // One sprinting tick resets the wait back to the full regen_delay_s and
    // drains rather than regens.
    tick_stamina(s, /*sprinting=*/true, 1.0f / 100.0f);
    REQUIRE(s.regen_cooldown_s == Catch::Approx(1.0f));
    REQUIRE(s.current == Catch::Approx(50.0f - 25.0f / 100.0f));

    // Now stopping has to wait the full delay again before any regen.
    for (int i = 0; i < 50; ++i) tick_stamina(s, /*sprinting=*/false, 1.0f / 100.0f);
    const f32 after_half = s.current;
    REQUIRE(s.regen_cooldown_s == Catch::Approx(0.5f));
    REQUIRE(after_half == Catch::Approx(50.0f - 25.0f / 100.0f));  // unchanged
}

TEST_CASE("gameplay: stamina_fraction reports current/max and guards max <= 0",
          "[gameplay][stamina]") {
    Stamina s = make_stamina();
    REQUIRE(stamina_fraction(s) == Catch::Approx(1.0f));
    s.current = 25.0f;
    REQUIRE(stamina_fraction(s) == Catch::Approx(0.25f));
    s.current = 0.0f;
    REQUIRE(stamina_fraction(s) == Catch::Approx(0.0f));

    // Degenerate capacity is guarded (no divide-by-zero / NaN).
    Stamina bad{0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 0.0f};
    REQUIRE(stamina_fraction(bad) == Catch::Approx(0.0f));
}

TEST_CASE("gameplay: tick_staminas regenerates an ECS Stamina",
          "[gameplay][stamina]") {
    World w;
    const Entity e = w.create();
    Stamina s = make_stamina();
    s.current = 40.0f;
    s.regen_cooldown_s = 0.0f;  // ready to regen
    w.add(e, s);

    // 1 s of world regen at 20/s -> 40 -> 60 (treated as not-sprinting).
    for (int i = 0; i < 100; ++i) tick_staminas(w, 1.0f / 100.0f);
    REQUIRE(w.get<Stamina>(e)->current == Catch::Approx(60.0f));
}

TEST_CASE("gameplay: stamina drain + regen is deterministic across runs",
          "[gameplay][determinism]") {
    const auto run = []() {
        Stamina s = make_stamina();
        constexpr f32 dt = 1.0f / 120.0f;
        // Sprint 2 s, coast 3 s (delay + regen), repeated.
        for (int rep = 0; rep < 3; ++rep) {
            for (int i = 0; i < 240; ++i) tick_stamina(s, /*sprinting=*/true, dt);
            for (int i = 0; i < 360; ++i) tick_stamina(s, /*sprinting=*/false, dt);
        }
        return std::vector<f32>{s.current, s.regen_cooldown_s};
    };
    REQUIRE(run() == run());
}
