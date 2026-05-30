// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/gameplay_reload.cpp — weapon reload / magazine state machine:
// firing consumes rounds and stops at empty, start_reload only when sensible,
// reloading blocks fire, a timed reload tops the mag up from the reserve by the
// right (possibly reserve-limited) amount, the ECS tick reloads, and the whole
// thing is deterministic.

#include "gameplay/GameplayComponents.h"
#include "gameplay/Reload.h"

#include "scene/World.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

using namespace psynder;
using namespace psynder::gameplay;
using psynder::scene::World;

namespace {

// A full mag: 30/90, 30-round cap, 2 s reload, not reloading.
Magazine full_mag() {
    return Magazine{30, 90, 30, 2.0f, 0.0f};
}

}  // namespace

TEST_CASE("gameplay: can_fire and consume_round drain the mag to empty",
          "[gameplay][reload]") {
    Magazine m = full_mag();
    REQUIRE(can_fire(m));

    REQUIRE(consume_round(m));
    REQUIRE(m.in_mag == 29);

    // Drain the rest.
    for (int i = 0; i < 29; ++i) REQUIRE(consume_round(m));
    REQUIRE(m.in_mag == 0);

    // Empty mag cannot fire and consume fails without going negative.
    REQUIRE_FALSE(can_fire(m));
    REQUIRE_FALSE(consume_round(m));
    REQUIRE(m.in_mag == 0);
    REQUIRE(m.reserve == 90);  // reserve untouched by firing
}

TEST_CASE("gameplay: start_reload only fires when it makes sense",
          "[gameplay][reload]") {
    // A full mag does not reload.
    Magazine full = full_mag();
    REQUIRE_FALSE(start_reload(full));
    REQUIRE_FALSE(reloading(full));

    // A partly-spent mag with reserve does reload.
    Magazine m{10, 90, 30, 2.0f, 0.0f};
    REQUIRE(start_reload(m));
    REQUIRE(reloading(m));
    REQUIRE(m.reload_left_s == Catch::Approx(2.0f));

    // Already reloading: a second call is rejected (timer not reset).
    REQUIRE_FALSE(start_reload(m));
    REQUIRE(m.reload_left_s == Catch::Approx(2.0f));

    // No reserve: nothing to reload from.
    Magazine dry{5, 0, 30, 2.0f, 0.0f};
    REQUIRE_FALSE(start_reload(dry));
    REQUIRE_FALSE(reloading(dry));
}

TEST_CASE("gameplay: a reloading magazine cannot fire", "[gameplay][reload]") {
    Magazine m{10, 90, 30, 2.0f, 0.0f};
    REQUIRE(start_reload(m));
    REQUIRE(reloading(m));
    REQUIRE_FALSE(can_fire(m));        // blocked mid-reload despite rounds loaded
    REQUIRE_FALSE(consume_round(m));
    REQUIRE(m.in_mag == 10);          // no round spent while reloading
}

TEST_CASE("gameplay: tick_reload completes after reload_time and tops up the mag",
          "[gameplay][reload]") {
    Magazine m{10, 90, 30, 2.0f, 0.0f};
    REQUIRE(start_reload(m));

    // Partway through: still reloading, mag unchanged.
    tick_reload(m, 1.0f);
    REQUIRE(reloading(m));
    REQUIRE(m.reload_left_s == Catch::Approx(1.0f));
    REQUIRE(m.in_mag == 10);

    // Cross the finish line: mag tops up from reserve, timer clamps to 0.
    tick_reload(m, 1.5f);  // overshoots the remaining 1.0 s
    REQUIRE_FALSE(reloading(m));
    REQUIRE(m.reload_left_s == Catch::Approx(0.0f));
    REQUIRE(m.in_mag == 30);          // refilled to mag_size
    REQUIRE(m.reserve == 70);         // 20 rounds drawn from reserve
    REQUIRE(can_fire(m));

    // A no-op when not reloading.
    tick_reload(m, 5.0f);
    REQUIRE(m.in_mag == 30);
    REQUIRE(m.reserve == 70);
}

TEST_CASE("gameplay: a reload is limited by the available reserve",
          "[gameplay][reload]") {
    // Mag is missing 25 rounds but only 8 are in reserve: only 8 move.
    Magazine m{5, 8, 30, 1.0f, 0.0f};
    REQUIRE(start_reload(m));
    tick_reload(m, 1.0f);  // exactly completes
    REQUIRE_FALSE(reloading(m));
    REQUIRE(m.in_mag == 13);          // 5 + 8
    REQUIRE(m.reserve == 0);          // reserve fully drained

    // With an empty reserve another reload can't start.
    REQUIRE_FALSE(start_reload(m));
}

TEST_CASE("gameplay: tick_reloads advances an ECS magazine", "[gameplay][reload]") {
    World w;
    const Entity e = w.create();
    w.add(e, Magazine{0, 30, 30, 2.0f, 0.0f});

    REQUIRE(start_reload(*w.get<Magazine>(e)));
    REQUIRE(reloading(*w.get<Magazine>(e)));

    // Two 1 s ticks complete the 2 s reload through the world walk.
    tick_reloads(w, 1.0f);
    REQUIRE(reloading(*w.get<Magazine>(e)));
    tick_reloads(w, 1.0f);
    REQUIRE_FALSE(reloading(*w.get<Magazine>(e)));
    REQUIRE(w.get<Magazine>(e)->in_mag == 30);
    REQUIRE(w.get<Magazine>(e)->reserve == 0);
}

TEST_CASE("gameplay: the reload cycle is deterministic across runs",
          "[gameplay][reload][determinism]") {
    const auto run = []() {
        Magazine m{30, 90, 30, 1.5f, 0.0f};
        constexpr f32 dt = 1.0f / 120.0f;
        // Empty the mag, reload to completion, repeat a few cycles.
        for (int cycle = 0; cycle < 3; ++cycle) {
            while (consume_round(m)) { /* fire until empty */ }
            start_reload(m);
            for (int step = 0; step < 200; ++step) tick_reload(m, dt);  // > 1.5 s
        }
        return Magazine{m.in_mag, m.reserve, m.mag_size, m.reload_time_s,
                        m.reload_left_s};
    };
    const Magazine a = run();
    const Magazine b = run();
    REQUIRE(a.in_mag == b.in_mag);
    REQUIRE(a.reserve == b.reserve);
    REQUIRE(a.reload_left_s == Catch::Approx(b.reload_left_s));
}
