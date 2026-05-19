// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/physics_core_smoke.cpp
//
// Lane 15 — physics-core smoke tests. Covers the M0/M1/M2 acceptance
// criteria from PLAN.md §3 row 15:
//   - A rigid body falls under gravity.
//   - A CharacterController moves in response to a CharacterInput.
//   - World tick is deterministic across two identical runs (precondition
//     for lockstep replay; tighter cross-platform check arrives in M6).
//
// The unit-test binary is gated OFF in the bootstrap build
// (PSYNDER_GX_BUILD_TESTS=OFF) so these never run yet — they exist to
// pin the contract once lane 25 re-enables psynder_unit.

#include "physics/core/PublicPhysicsCore.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace psynder::physics;

namespace {

constexpr float kDt   = 1.0f / 120.0f; // matches the design's fixed tick
constexpr int   kTicks = 240;          // 2 seconds of sim

// Small WorldDesc suitable for unit tests.  The production default
// (max_bodies = 64k) causes Jolt's temp allocator (16 MB) to be
// exhausted during the first tick because the body/pair buffer
// reservation alone exceeds that budget at that scale.  Unit tests
// never need more than a handful of bodies.
WorldDesc unit_world_desc() {
    WorldDesc d{};
    d.max_bodies      = 256u;
    d.max_constraints = 256u;
    d.tick_hz         = 120u;
    return d;
}

// Tiny scope guard so each test starts with a fresh world.
struct WorldScope {
    World* w = nullptr;
    WorldScope() {
        w = create_world(unit_world_desc());
        REQUIRE(w != nullptr);
    }
    ~WorldScope() { destroy_world(w); }
};

} // namespace

TEST_CASE("physics-core: world creates and destroys cleanly",
          "[physics][smoke]") {
    WorldDesc desc = unit_world_desc();
    World* w = create_world(desc);
    REQUIRE(w != nullptr);
    destroy_world(w);

    // Re-create after destroy — exercises the global-init refcount path.
    World* w2 = create_world(desc);
    REQUIRE(w2 != nullptr);
    destroy_world(w2);
}

TEST_CASE("physics-core: sphere falls under gravity",
          "[physics][rigidbody]") {
    WorldScope scope;

    // 100 m × 1 m × 100 m static box at y = -1 acts as the floor.
    BodyDesc floor{};
    floor.shape    = Shape::Box;
    floor.pos[0]   = 0.0f;
    floor.pos[1]   = -1.0f;
    floor.pos[2]   = 0.0f;
    floor.rot_quat[3] = 1.0f;
    floor.mass_kg  = 0.0f; // static
    floor.dims[0]  = 50.0f;
    floor.dims[1]  = 0.5f;
    floor.dims[2]  = 50.0f;
    RigidBody* floor_body = create_body(scope.w, floor);
    REQUIRE(floor_body != nullptr);

    // 0.5 m radius sphere starting at y = 10 m, 1 kg.
    BodyDesc ball{};
    ball.shape    = Shape::Sphere;
    ball.pos[0]   = 0.0f;
    ball.pos[1]   = 10.0f;
    ball.pos[2]   = 0.0f;
    ball.rot_quat[3] = 1.0f;
    ball.mass_kg  = 1.0f;
    ball.dims[0]  = 0.5f;
    RigidBody* ball_body = create_body(scope.w, ball);
    REQUIRE(ball_body != nullptr);

    float pos[3]{0,0,0};
    float quat[4]{0,0,0,1};
    body_get_transform(ball_body, pos, quat);
    const float start_y = pos[1];

    for (int i = 0; i < kTicks; ++i) {
        tick(scope.w, kDt);
    }

    body_get_transform(ball_body, pos, quat);
    const float end_y = pos[1];

    // Gravity pulls the ball down by many metres in 2 s of sim. We
    // don't pin the exact landing height (depends on Jolt contact
    // resolution + restitution); just assert significant descent and
    // that the ball is at rest on the floor (above the half-extent).
    REQUIRE(end_y < start_y - 1.0f);
    REQUIRE(end_y > -1.0f); // didn't tunnel through the floor

    destroy_body(scope.w, ball_body);
    destroy_body(scope.w, floor_body);
}

TEST_CASE("physics-core: applied force moves a body",
          "[physics][rigidbody]") {
    WorldScope scope;

    BodyDesc d{};
    d.shape    = Shape::Sphere;
    d.pos[0]   = 0.0f;
    d.pos[1]   = 100.0f;  // float in vacuum, away from any floor
    d.pos[2]   = 0.0f;
    d.rot_quat[3] = 1.0f;
    d.mass_kg  = 1.0f;
    d.dims[0]  = 0.25f;
    RigidBody* b = create_body(scope.w, d);
    REQUIRE(b != nullptr);

    // Push +X every tick for 30 frames. Gravity is also acting, but
    // the +X displacement should still be unambiguous.
    for (int i = 0; i < 30; ++i) {
        body_apply_force(b, 100.0f, 0.0f, 0.0f);
        tick(scope.w, kDt);
    }
    float pos[3]{0,0,0};
    float quat[4]{0,0,0,1};
    body_get_transform(b, pos, quat);
    REQUIRE(pos[0] > 0.05f);

    destroy_body(scope.w, b);
}

TEST_CASE("physics-core: character controller responds to input",
          "[physics][character]") {
    WorldScope scope;

    // Ground for the character to stand on.
    BodyDesc floor{};
    floor.shape    = Shape::Box;
    floor.pos[1]   = -0.5f;
    floor.rot_quat[3] = 1.0f;
    floor.dims[0]  = 50.0f;
    floor.dims[1]  = 0.5f;
    floor.dims[2]  = 50.0f;
    RigidBody* floor_body = create_body(scope.w, floor);
    REQUIRE(floor_body != nullptr);

    CharacterDesc cd{};
    cd.pos[0] = 0.0f;
    cd.pos[1] = 2.0f;
    cd.pos[2] = 0.0f;
    CharacterController* cc = create_character(scope.w, cd);
    REQUIRE(cc != nullptr);

    // Let the character settle on the ground a moment.
    CharacterInput idle{};
    for (int i = 0; i < 60; ++i) {
        character_tick(cc, idle, kDt);
        tick(scope.w, kDt);
    }

    float pos0[3]{};
    character_get_transform(cc, pos0, 0.0f);

    // Drive forward (+Z) for half a second.
    CharacterInput fwd{};
    fwd.move_xy[0] = 0.0f;
    fwd.move_xy[1] = 1.0f;
    for (int i = 0; i < 60; ++i) {
        character_tick(cc, fwd, kDt);
        tick(scope.w, kDt);
    }

    float pos1[3]{};
    character_get_transform(cc, pos1, 0.0f);
    REQUIRE(pos1[2] > pos0[2] + 0.5f);

    destroy_character(scope.w, cc);
    destroy_body(scope.w, floor_body);
}

TEST_CASE("physics-core: identical worlds produce identical traces "
          "(determinism precondition)",
          "[physics][determinism]") {
    // Two identical worlds with identical inputs must produce identical
    // body positions. This is the local-determinism check; the cross-
    // platform check (arm64 vs x86_64 bit-identity) is wired into the
    // CI matrix when lane 18 (net) lockstep tests come online.
    auto run = [](float* out_y_trace, int n) {
        World* w = create_world(unit_world_desc());
        BodyDesc d{};
        d.shape    = Shape::Sphere;
        d.pos[1]   = 5.0f;
        d.rot_quat[3] = 1.0f;
        d.mass_kg  = 1.0f;
        d.dims[0]  = 0.5f;
        RigidBody* b = create_body(w, d);
        for (int i = 0; i < n; ++i) {
            tick(w, kDt);
            float p[3], q[4];
            body_get_transform(b, p, q);
            out_y_trace[i] = p[1];
        }
        destroy_body(w, b);
        destroy_world(w);
    };

    constexpr int n = 60;
    float trace_a[n]{};
    float trace_b[n]{};
    run(trace_a, n);
    run(trace_b, n);
    for (int i = 0; i < n; ++i) {
        // Bit-exact equality required for lockstep determinism.
        REQUIRE(trace_a[i] == trace_b[i]);
    }
}
