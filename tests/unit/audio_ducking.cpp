// SPDX-License-Identifier: MIT
// Psynder — lane 14 unit test: sidechain ducking envelope (Ducking.h), the
// compressor-style auto-duck that lowers one bus while a louder trigger plays.
//
// Verifies the load-bearing properties of engine/audio/Ducking.h:
//   (a) duck_init leaves the envelope at unity (no ducking);
//   (b) with the trigger active the gain ducks DOWN toward the floor and
//       converges onto it without ever dropping below it;
//   (c) releasing (trigger off) returns the gain UP to 1.0;
//   (d) a faster attack reaches the floor in fewer steps than a slower release
//       takes to return to unity;
//   (e) a single step never overshoots its target (down or up);
//   (f) a non-finite or zero/negative dt performs no move (guard);
//   (g) duck_target reports floor when active, unity when not; and
//   (h) determinism: identical step sequences produce bit-identical gains.

#include "audio/Ducking.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <limits>

using namespace psynder;
using namespace psynder::audio;

namespace {

// Step an envelope `n` times with a fixed dt and trigger state.
void run_steps(DuckEnvelope& e, const DuckParams& p, bool trigger_active,
               f32 dt_s, int n) noexcept {
    for (int i = 0; i < n; ++i) {
        duck_update(e, p, trigger_active, dt_s);
    }
}

}  // namespace

TEST_CASE("audio: duck_init leaves the envelope at unity", "[audio][ducking]") {
    DuckEnvelope e{};
    duck_init(e);
    REQUIRE(e.gain == Catch::Approx(1.0f).margin(1e-6f));
    REQUIRE(duck_gain(e) == Catch::Approx(1.0f).margin(1e-6f));
}

TEST_CASE("audio: an active trigger ducks the gain down to the floor", "[audio][ducking]") {
    const DuckParams p{0.25f, 4.0f, 1.0f};  // floor 0.25, fast attack
    DuckEnvelope e{};
    duck_init(e);

    // One block at the attack rate moves down by attack_per_s * dt.
    duck_update(e, p, /*trigger_active=*/true, 0.1f);
    INFO("after one attack step gain=" << e.gain);
    REQUIRE(e.gain < 1.0f);            // it has started ducking
    REQUIRE(e.gain >= p.duck_floor);   // but never below the floor

    // Drive it long enough to converge, then confirm it sits ON the floor and
    // never undershoots no matter how many more steps run.
    run_steps(e, p, /*trigger_active=*/true, 0.1f, 100);
    REQUIRE(e.gain == Catch::Approx(p.duck_floor).margin(1e-6f));
    run_steps(e, p, /*trigger_active=*/true, 0.1f, 50);
    REQUIRE(e.gain >= p.duck_floor);
    REQUIRE(e.gain == Catch::Approx(p.duck_floor).margin(1e-6f));
}

TEST_CASE("audio: releasing the trigger returns the gain to unity", "[audio][ducking]") {
    const DuckParams p{0.2f, 6.0f, 2.0f};
    DuckEnvelope e{};
    duck_init(e);

    // First fully duck.
    run_steps(e, p, /*trigger_active=*/true, 0.05f, 200);
    REQUIRE(e.gain == Catch::Approx(p.duck_floor).margin(1e-6f));

    // Now release: the gain climbs back up and converges exactly on unity.
    run_steps(e, p, /*trigger_active=*/false, 0.05f, 200);
    INFO("after release gain=" << e.gain);
    REQUIRE(e.gain == Catch::Approx(1.0f).margin(1e-6f));
    REQUIRE(e.gain <= 1.0f);  // never overshoots past unity
}

TEST_CASE("audio: a faster attack reaches the floor before the release recovers", "[audio][ducking]") {
    // Attack is faster than release, so ducking DOWN takes fewer steps than
    // releasing the same span back UP.
    const DuckParams p{0.0f, 5.0f, 1.0f};  // floor 0, attack 5x the release
    const f32 dt = 0.01f;

    // Count steps to duck from unity onto the floor.
    DuckEnvelope down{};
    duck_init(down);
    int attack_steps = 0;
    while (down.gain > p.duck_floor + 1e-6f && attack_steps < 100000) {
        duck_update(down, p, /*trigger_active=*/true, dt);
        ++attack_steps;
    }

    // Count steps to release from the floor back to unity.
    DuckEnvelope up{};
    up.gain = p.duck_floor;
    int release_steps = 0;
    while (up.gain < 1.0f - 1e-6f && release_steps < 100000) {
        duck_update(up, p, /*trigger_active=*/false, dt);
        ++release_steps;
    }

    INFO("attack_steps=" << attack_steps << " release_steps=" << release_steps);
    REQUIRE(attack_steps < release_steps);
}

TEST_CASE("audio: a single step never overshoots its target", "[audio][ducking]") {
    // A huge rate * dt would blow past the target in one block if unclamped;
    // the envelope must snap exactly onto the target instead.
    const DuckParams p{0.3f, 1000.0f, 1000.0f};

    // Ducking down: lands exactly on the floor, not below.
    DuckEnvelope d{};
    duck_init(d);
    duck_update(d, p, /*trigger_active=*/true, 1.0f);
    REQUIRE(d.gain == Catch::Approx(p.duck_floor).margin(1e-6f));
    REQUIRE(d.gain >= p.duck_floor);

    // Releasing up: lands exactly on unity, not above.
    DuckEnvelope u{};
    u.gain = p.duck_floor;
    duck_update(u, p, /*trigger_active=*/false, 1.0f);
    REQUIRE(u.gain == Catch::Approx(1.0f).margin(1e-6f));
    REQUIRE(u.gain <= 1.0f);
}

TEST_CASE("audio: a non-finite or non-positive dt makes no move", "[audio][ducking]") {
    const DuckParams p{0.1f, 3.0f, 3.0f};
    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    const f32 inf = std::numeric_limits<f32>::infinity();

    // Start partway through a duck so a stray move would be visible either way.
    DuckEnvelope e{};
    e.gain = 0.6f;

    duck_update(e, p, /*trigger_active=*/true, 0.0f);
    REQUIRE(e.gain == Catch::Approx(0.6f).margin(1e-6f));

    duck_update(e, p, /*trigger_active=*/true, -0.1f);
    REQUIRE(e.gain == Catch::Approx(0.6f).margin(1e-6f));

    duck_update(e, p, /*trigger_active=*/true, nan);
    REQUIRE(e.gain == Catch::Approx(0.6f).margin(1e-6f));

    duck_update(e, p, /*trigger_active=*/false, inf);
    REQUIRE(e.gain == Catch::Approx(0.6f).margin(1e-6f));
}

TEST_CASE("audio: duck_target is the floor when active and unity when not", "[audio][ducking]") {
    const DuckParams p{0.35f, 2.0f, 2.0f};
    REQUIRE(duck_target(p, /*trigger_active=*/true) == Catch::Approx(0.35f).margin(1e-6f));
    REQUIRE(duck_target(p, /*trigger_active=*/false) == Catch::Approx(1.0f).margin(1e-6f));

    // A floor outside [0,1] is clamped so the target stays well-formed.
    const DuckParams hi{1.5f, 2.0f, 2.0f};
    REQUIRE(duck_target(hi, /*trigger_active=*/true) == Catch::Approx(1.0f).margin(1e-6f));
    const DuckParams lo{-0.5f, 2.0f, 2.0f};
    REQUIRE(duck_target(lo, /*trigger_active=*/true) == Catch::Approx(0.0f).margin(1e-6f));
}

TEST_CASE("audio: ducking is deterministic across identical sequences", "[audio][ducking]") {
    const DuckParams p{0.15f, 3.5f, 1.25f};
    const f32 dt = 0.02f;

    // Same params + same trigger pattern + same dt => bit-identical gains.
    DuckEnvelope a{};
    DuckEnvelope b{};
    duck_init(a);
    duck_init(b);

    const bool pattern[] = {true, true, true, false, false, true, false, true, true, false};
    for (const bool active : pattern) {
        for (int i = 0; i < 7; ++i) {
            duck_update(a, p, active, dt);
            duck_update(b, p, active, dt);
        }
        REQUIRE(a.gain == b.gain);  // exact equality, not approx
    }
    REQUIRE(duck_gain(a) == duck_gain(b));
}
