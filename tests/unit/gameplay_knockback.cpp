// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/gameplay_knockback.cpp — deterministic radial knockback:
// away-from-source direction, distance falloff (stronger up close, zero at /
// beyond the radius), the at-source degenerate case, component accumulation,
// damped decay toward rest without overshoot, the radial applier (in-range
// entity shoved away, out-of-range one ignored), and cross-world determinism.

#include "gameplay/GameplayComponents.h"
#include "gameplay/Knockback.h"

#include "scene/GxComponents.h"
#include "scene/World.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

using namespace psynder;
using namespace psynder::gameplay;
using psynder::scene::TransformWS;
using psynder::scene::World;

namespace {

// A Knockback entity (with a world transform) at `pos`. damping defaults off so
// apply_radial_knockback tests read the raw accumulated velocity.
Entity spawn_kb(World& w, math::Vec3 pos, f32 damping = 0.0f) {
    const Entity e = w.create();
    w.add(e, Knockback{{0.0f, 0.0f, 0.0f}, damping});
    TransformWS t{};
    t.mtw = math::translate(pos);
    t.prev_mtw = t.mtw;
    w.add(e, t);
    return e;
}

}  // namespace

TEST_CASE("gameplay: knockback impulse points away from the source",
          "[gameplay]") {
    // Source at origin, target on +X -> impulse pushes further along +X.
    const math::Vec3 imp =
        knockback_impulse({3.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, 100.0f, 10.0f);
    REQUIRE(imp.x > 0.0f);
    REQUIRE(imp.y == Catch::Approx(0.0f));
    REQUIRE(imp.z == Catch::Approx(0.0f));

    // A blast below the target launches it upward (vertical component honoured).
    const math::Vec3 up =
        knockback_impulse({0.0f, 2.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, 100.0f, 10.0f);
    REQUIRE(up.y > 0.0f);
    REQUIRE(up.x == Catch::Approx(0.0f));
    REQUIRE(up.z == Catch::Approx(0.0f));
}

TEST_CASE("gameplay: knockback is stronger closer to the source",
          "[gameplay]") {
    const math::Vec3 close =
        knockback_impulse({1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, 100.0f, 10.0f);
    const math::Vec3 distant =
        knockback_impulse({8.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, 100.0f, 10.0f);
    REQUIRE(math::length(close) > math::length(distant));
    // dist 1, radius 10 -> 100 * (1 - 0.1) = 90 m/s along +X.
    REQUIRE(close.x == Catch::Approx(90.0f));
    // dist 8, radius 10 -> 100 * (1 - 0.8) = 20 m/s.
    REQUIRE(distant.x == Catch::Approx(20.0f));
}

TEST_CASE("gameplay: knockback is zero at and beyond the radius",
          "[gameplay]") {
    const math::Vec3 at =
        knockback_impulse({10.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, 100.0f, 10.0f);
    REQUIRE(math::length(at) == Catch::Approx(0.0f));

    const math::Vec3 beyond =
        knockback_impulse({15.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, 100.0f, 10.0f);
    REQUIRE(math::length(beyond) == Catch::Approx(0.0f));
}

TEST_CASE("gameplay: knockback at the source is the documented zero, no NaN",
          "[gameplay]") {
    const math::Vec3 imp =
        knockback_impulse({5.0f, 1.0f, -2.0f}, {5.0f, 1.0f, -2.0f}, 100.0f, 10.0f);
    REQUIRE(std::isfinite(imp.x));
    REQUIRE(std::isfinite(imp.y));
    REQUIRE(std::isfinite(imp.z));
    REQUIRE(imp.x == Catch::Approx(0.0f));
    REQUIRE(imp.y == Catch::Approx(0.0f));
    REQUIRE(imp.z == Catch::Approx(0.0f));
}

TEST_CASE("gameplay: a non-positive knockback radius pushes nothing",
          "[gameplay]") {
    const math::Vec3 imp =
        knockback_impulse({1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, 100.0f, 0.0f);
    REQUIRE(math::length(imp) == Catch::Approx(0.0f));
}

TEST_CASE("gameplay: add_knockback accumulates impulses additively",
          "[gameplay]") {
    Knockback k{{0.0f, 0.0f, 0.0f}, 0.0f};
    add_knockback(k, {1.0f, 2.0f, 3.0f});
    add_knockback(k, {4.0f, -1.0f, 0.5f});
    REQUIRE(k.velocity.x == Catch::Approx(5.0f));
    REQUIRE(k.velocity.y == Catch::Approx(1.0f));
    REQUIRE(k.velocity.z == Catch::Approx(3.5f));
}

TEST_CASE("gameplay: tick_knockback decays velocity toward zero, no overshoot",
          "[gameplay]") {
    // damping 10 (m/s)/s, dt 0.5 -> bleed 5 m/s off each component's magnitude.
    Knockback k{{8.0f, -8.0f, 1.0f}, 10.0f};
    tick_knockback(k, 0.5f);
    REQUIRE(k.velocity.x == Catch::Approx(3.0f));   // 8 - 5
    REQUIRE(k.velocity.y == Catch::Approx(-3.0f));  // -8 + 5
    // The small +1 component would cross zero — clamp to 0, never flip sign.
    REQUIRE(k.velocity.z == Catch::Approx(0.0f));

    // Keep ticking: it converges to rest and stays there (no negative drift).
    tick_knockback(k, 1.0f);  // bleed 10 -> both 3 / -3 clamp to 0
    REQUIRE(k.velocity.x == Catch::Approx(0.0f));
    REQUIRE(k.velocity.y == Catch::Approx(0.0f));
    REQUIRE(k.velocity.z == Catch::Approx(0.0f));
}

TEST_CASE("gameplay: tick_knockback guards a zero or non-finite dt",
          "[gameplay]") {
    Knockback k{{4.0f, 0.0f, 0.0f}, 10.0f};
    tick_knockback(k, 0.0f);  // no-op
    REQUIRE(k.velocity.x == Catch::Approx(4.0f));
    tick_knockback(k, -0.5f);  // no-op
    REQUIRE(k.velocity.x == Catch::Approx(4.0f));
    tick_knockback(k, std::nan(""));  // no-op (non-finite)
    REQUIRE(k.velocity.x == Catch::Approx(4.0f));
}

TEST_CASE("gameplay: apply_radial_knockback shoves in-range, ignores out-of-range",
          "[gameplay]") {
    World w;
    const Entity in = spawn_kb(w, {3.0f, 0.0f, 0.0f});    // 3 m from blast
    const Entity out = spawn_kb(w, {20.0f, 0.0f, 0.0f});  // 20 m -> out of range

    apply_radial_knockback(w, {0.0f, 0.0f, 0.0f}, 100.0f, 10.0f);

    const Knockback* ki = w.get<Knockback>(in);
    REQUIRE(ki != nullptr);
    REQUIRE(ki->velocity.x > 0.0f);  // pushed away from the origin, along +X
    REQUIRE(ki->velocity.y == Catch::Approx(0.0f));
    REQUIRE(ki->velocity.z == Catch::Approx(0.0f));
    // dist 3, radius 10 -> 100 * (1 - 0.3) = 70 m/s.
    REQUIRE(ki->velocity.x == Catch::Approx(70.0f));

    const Knockback* ko = w.get<Knockback>(out);
    REQUIRE(ko != nullptr);
    REQUIRE(math::length(ko->velocity) == Catch::Approx(0.0f));  // untouched
}

TEST_CASE("gameplay: apply_radial_knockback pushes each victim along its own ray",
          "[gameplay]") {
    World w;
    // Spawn in a scrambled order; assert each by its captured id so the result
    // cannot depend on spawn / iteration order.
    const Entity east = spawn_kb(w, {2.0f, 0.0f, 0.0f});
    const Entity north = spawn_kb(w, {0.0f, 0.0f, 2.0f});
    const Entity up = spawn_kb(w, {0.0f, 2.0f, 0.0f});

    apply_radial_knockback(w, {0.0f, 0.0f, 0.0f}, 100.0f, 10.0f);

    REQUIRE(w.get<Knockback>(east)->velocity.x > 0.0f);
    REQUIRE(w.get<Knockback>(north)->velocity.z > 0.0f);
    REQUIRE(w.get<Knockback>(up)->velocity.y > 0.0f);
}

TEST_CASE("gameplay: radial knockback is deterministic across worlds",
          "[gameplay][determinism]") {
    const auto run = []() {
        World w;
        std::vector<Entity> victims;
        for (int i = 0; i < 6; ++i) {
            const f32 f = static_cast<f32>(i);
            victims.push_back(spawn_kb(w, {f, f * 0.5f, -f * 0.25f}));
        }
        // Several blasts at different points (no RNG anywhere).
        apply_radial_knockback(w, {0.0f, 0.0f, 0.0f}, 80.0f, 8.0f);
        apply_radial_knockback(w, {3.0f, 0.0f, 0.0f}, 80.0f, 8.0f);
        apply_radial_knockback(w, {1.0f, 1.0f, -1.0f}, 80.0f, 8.0f);

        std::vector<f32> vel;
        for (Entity e : victims) {
            const math::Vec3 v = w.get<Knockback>(e)->velocity;
            vel.push_back(v.x);
            vel.push_back(v.y);
            vel.push_back(v.z);
        }
        return vel;
    };
    const std::vector<f32> a = run();
    const std::vector<f32> b = run();
    REQUIRE(a == b);  // bit-identical knockback velocities across two worlds
}
