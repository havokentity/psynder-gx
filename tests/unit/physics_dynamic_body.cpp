// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/physics_dynamic_body.cpp
//
// Smoke + determinism coverage for the dynamic Jolt rigid-body spine
// (engine/physics/core/DynamicBody.{h,cpp}), ADR-019 class 2. Mirrors the
// physics character-spine test pattern: a RAII world scope, a fixed-tick step
// loop, and metric-unit assertions (1 unit = 1 m, real gravity).

#include "physics/core/DynamicBody.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

namespace dyn = psynder::physics::dynamic_body;

namespace {

constexpr int kOneSecondTicks = 120;

struct WorldScope {
    dyn::World* world = nullptr;

    WorldScope() {
        dyn::WorldDesc desc{};
        desc.max_bodies = 64;
        desc.tick_hz = 120;
        world = dyn::create_world(desc);
        REQUIRE(world != nullptr);
    }

    ~WorldScope() { dyn::destroy_world(world); }
};

void step(dyn::World* world, int ticks) {
    for (int i = 0; i < ticks; ++i) {
        dyn::step_fixed(world);
    }
}

}  // namespace

TEST_CASE("physics-dynamic-body: box falls under gravity and rests near floor",
          "[physics][dynamic]") {
    WorldScope scope;
    REQUIRE(dyn::add_static_ground(scope.world, 12.0f, 12.0f).valid());

    // Reject invalid descs (non-positive mass / extent) up front.
    REQUIRE_FALSE(dyn::create_dynamic_box(scope.world, {0.5f, 0.5f, 0.5f},
                                          {0.0f, 3.0f, 0.0f}, 0.0f)
                      .valid());
    REQUIRE_FALSE(dyn::create_dynamic_box(scope.world, {0.0f, 0.5f, 0.5f},
                                          {0.0f, 3.0f, 0.0f}, 8.0f)
                      .valid());

    const dyn::DynamicBodyHandle handle = dyn::create_dynamic_box(
        scope.world, {0.5f, 0.5f, 0.5f}, {0.0f, 3.0f, 0.0f}, 8.0f);
    REQUIRE(handle.valid());

    const float start_y = dyn::dynamic_body_position(scope.world, handle).y;
    REQUIRE(start_y == Catch::Approx(3.0f).margin(1.0e-4f));

    step(scope.world, 3 * kOneSecondTicks);  // ~3 s to fall and settle.

    const psynder::math::Vec3 end =
        dyn::dynamic_body_position(scope.world, handle);
    REQUIRE(end.y < start_y - 1.0f);  // it fell a long way.
    // Half-extent 0.5 resting on a floor whose top face is at y = 0 puts the
    // centre near y = 0.5; it did NOT tunnel through to negative y.
    REQUIRE(end.y > 0.2f);
    REQUIRE(end.y < 0.9f);
}

TEST_CASE("physics-dynamic-body: sphere falls and an upward impulse raises it",
          "[physics][dynamic]") {
    WorldScope scope;
    REQUIRE(dyn::add_static_ground(scope.world, 12.0f, 12.0f).valid());

    REQUIRE_FALSE(dyn::create_dynamic_sphere(scope.world, 0.0f,
                                             {0.0f, 2.0f, 0.0f}, 5.0f)
                      .valid());

    const dyn::DynamicBodyHandle handle = dyn::create_dynamic_sphere(
        scope.world, 0.5f, {0.0f, 2.0f, 0.0f}, 5.0f);
    REQUIRE(handle.valid());

    step(scope.world, 2 * kOneSecondTicks);  // let it settle on the floor.
    const float rest_y = dyn::dynamic_body_position(scope.world, handle).y;
    REQUIRE(rest_y > 0.2f);
    REQUIRE(rest_y < 0.9f);

    // A strong upward impulse (kg·m/s) should pop the resting sphere up.
    dyn::apply_impulse(scope.world, handle, {0.0f, 40.0f, 0.0f});

    float peak_y = rest_y;
    for (int i = 0; i < kOneSecondTicks; ++i) {
        dyn::step_fixed(scope.world);
        const float y = dyn::dynamic_body_position(scope.world, handle).y;
        if (y > peak_y) peak_y = y;
    }
    REQUIRE(peak_y > rest_y + 0.5f);  // the impulse lifted it clear of rest.
}

TEST_CASE("physics-dynamic-body: horizontal impulse pushes a body then remove drops it",
          "[physics][dynamic]") {
    WorldScope scope;
    REQUIRE(dyn::add_static_ground(scope.world, 12.0f, 12.0f).valid());

    const dyn::DynamicBodyHandle handle = dyn::create_dynamic_box(
        scope.world, {0.5f, 0.5f, 0.5f}, {0.0f, 0.5f, 0.0f}, 6.0f);
    REQUIRE(handle.valid());

    step(scope.world, 30);  // settle on the ground.
    const float start_x = dyn::dynamic_body_position(scope.world, handle).x;

    dyn::apply_impulse(scope.world, handle, {30.0f, 0.0f, 0.0f});  // kick +X.
    step(scope.world, 60);

    REQUIRE(dyn::dynamic_body_position(scope.world, handle).x > start_x + 0.3f);

    // Lifecycle: remove succeeds once, then the body is gone; invalid handles
    // are rejected; position of a removed handle reports the origin + !exists.
    REQUIRE(dyn::dynamic_body_exists(scope.world, handle));
    REQUIRE(dyn::remove_dynamic_body(scope.world, handle));
    REQUIRE_FALSE(dyn::dynamic_body_exists(scope.world, handle));
    REQUIRE_FALSE(dyn::remove_dynamic_body(scope.world, handle));
    REQUIRE_FALSE(
        dyn::remove_dynamic_body(scope.world, dyn::DynamicBodyHandle{}));

    const psynder::math::Vec3 gone =
        dyn::dynamic_body_position(scope.world, handle);
    REQUIRE(gone.x == Catch::Approx(0.0f).margin(1.0e-6f));
    REQUIRE(gone.y == Catch::Approx(0.0f).margin(1.0e-6f));
    REQUIRE(gone.z == Catch::Approx(0.0f).margin(1.0e-6f));
}

TEST_CASE("physics-dynamic-body: gravity fall trajectory is deterministic across worlds",
          "[physics][dynamic]") {
    WorldScope a;
    WorldScope b;
    REQUIRE(dyn::add_static_ground(a.world, 12.0f, 12.0f).valid());
    REQUIRE(dyn::add_static_ground(b.world, 12.0f, 12.0f).valid());

    const dyn::DynamicBodyHandle ha = dyn::create_dynamic_box(
        a.world, {0.5f, 0.5f, 0.5f}, {0.0f, 4.0f, 0.0f}, 8.0f);
    const dyn::DynamicBodyHandle hb = dyn::create_dynamic_box(
        b.world, {0.5f, 0.5f, 0.5f}, {0.0f, 4.0f, 0.0f}, 8.0f);
    REQUIRE(ha.valid());
    REQUIRE(hb.valid());

    // Lockstep the two worlds and compare the full Y trajectory tick-by-tick.
    for (int tick = 0; tick < 2 * kOneSecondTicks; ++tick) {
        dyn::step_fixed(a.world);
        dyn::step_fixed(b.world);
        const psynder::math::Vec3 pa = dyn::dynamic_body_position(a.world, ha);
        const psynder::math::Vec3 pb = dyn::dynamic_body_position(b.world, hb);
        REQUIRE(pa.x == Catch::Approx(pb.x).margin(1.0e-5f));
        REQUIRE(pa.y == Catch::Approx(pb.y).margin(1.0e-5f));
        REQUIRE(pa.z == Catch::Approx(pb.z).margin(1.0e-5f));
    }
}
