// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/player_movement.cpp — the Quake3-style deterministic movement
// kernel (engine/physics/core/PlayerMovement). Proves the canonical arena feel:
// friction brings a player to a crisp stop, ground acceleration caps at the run
// speed, AIR acceleration lets a strafing player exceed it (air-control /
// bunnyhop), a jump preserves horizontal speed, crouch slows you, and the whole
// thing is bit-deterministic.

#include "physics/core/PlayerMovement.h"

#include "math/Math.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cmath>
#include <vector>

using namespace psynder;
using physics::MoveCmd;
using physics::MoveState;
using physics::MoveTuning;
using physics::pm_move;

namespace {
constexpr f32 kDt = 1.0f / 128.0f;
f32 hspeed(const math::Vec3& v) { return std::sqrt(v.x * v.x + v.z * v.z); }
}  // namespace

TEST_CASE("movement: ground friction brings a player to a crisp stop",
          "[movement][gameplay]") {
    MoveTuning t{};
    MoveState s{};
    s.grounded = true;
    s.velocity = {t.max_speed_mps, 0.0f, 0.0f};  // running, then release input
    MoveCmd stop{};                              // no wish_dir, no jump
    f32 prev = hspeed(s.velocity);
    for (int i = 0; i < 256; ++i) {
        pm_move(s, stop, t, kDt);
        const f32 cur = hspeed(s.velocity);
        REQUIRE(cur <= prev + 1e-5f);  // monotonic non-increasing
        prev = cur;
    }
    REQUIRE(hspeed(s.velocity) == Catch::Approx(0.0f).margin(1e-4f));
}

TEST_CASE("movement: ground acceleration ramps to the run cap and never exceeds it",
          "[movement][gameplay]") {
    MoveTuning t{};
    MoveState s{};
    s.grounded = true;
    MoveCmd fwd{};
    fwd.wish_dir = {1.0f, 0.0f, 0.0f};  // hold forward, full speed
    for (int i = 0; i < 512; ++i) {
        pm_move(s, fwd, t, kDt);
        REQUIRE(hspeed(s.velocity) <= t.max_speed_mps + 1e-3f);  // capped
    }
    REQUIRE(hspeed(s.velocity) == Catch::Approx(t.max_speed_mps).margin(1e-2f));
}

TEST_CASE("movement: air-strafing exceeds the ground run cap (air control)",
          "[movement][gameplay]") {
    MoveTuning t{};
    MoveState s{};
    s.grounded = false;                          // airborne
    s.velocity = {t.max_speed_mps, 0.0f, 0.0f};  // already at the ground cap
    // Each tick steer perpendicular to the current horizontal velocity — the
    // classic air-strafe: the addspeed-vs-dot mechanic keeps adding speed since
    // the wish dir is never aligned with the velocity.
    const f32 start = hspeed(s.velocity);
    f32 prev = start;
    for (int i = 0; i < 256; ++i) {
        MoveCmd strafe{};
        strafe.wish_dir = {-s.velocity.z, 0.0f, s.velocity.x};  // 90° to vel
        pm_move(s, strafe, t, kDt);
        const f32 cur = hspeed(s.velocity);
        REQUIRE(cur >= prev - 1e-6f);  // never loses speed in air (no friction)
        prev = cur;
    }
    // Strictly faster than the ground cap now — air control gained speed.
    REQUIRE(hspeed(s.velocity) > t.max_speed_mps + 1e-2f);
}

TEST_CASE("movement: a jump preserves horizontal speed (bunnyhop invariant)",
          "[movement][gameplay]") {
    MoveTuning t{};
    MoveState s{};
    s.grounded = true;
    s.velocity = {t.max_speed_mps, 0.0f, 0.0f};
    const f32 before = hspeed(s.velocity);
    MoveCmd jump{};
    jump.wish_dir = {1.0f, 0.0f, 0.0f};
    jump.jump = true;
    pm_move(s, jump, t, kDt);
    REQUIRE(s.grounded == false);
    // The launch sets vertical velocity to jump_speed; gravity then acts for
    // this same airborne tick, so vy = jump_speed - g*dt (one tick of fall).
    REQUIRE(s.velocity.y ==
            Catch::Approx(t.jump_speed_mps - t.gravity_mps2 * kDt));
    // Friction was skipped on the jump tick, so horizontal speed is preserved
    // (ground accel can't push past the cap, so it stays right at it).
    REQUIRE(hspeed(s.velocity) >= before - 1e-4f);
    REQUIRE(hspeed(s.velocity) <= t.max_speed_mps + 1e-3f);
}

TEST_CASE("movement: crouching caps speed at the duck scale", "[movement][gameplay]") {
    MoveTuning t{};
    MoveState s{};
    s.grounded = true;
    MoveCmd crouch_fwd{};
    crouch_fwd.wish_dir = {1.0f, 0.0f, 0.0f};
    crouch_fwd.crouch = true;
    for (int i = 0; i < 512; ++i) pm_move(s, crouch_fwd, t, kDt);
    REQUIRE(hspeed(s.velocity) ==
            Catch::Approx(t.max_speed_mps * t.duck_speed_scale).margin(1e-2f));
}

TEST_CASE("movement: the kernel is bit-deterministic across runs",
          "[movement][determinism]") {
    const auto run = []() {
        MoveTuning t{};
        MoveState s{};
        s.grounded = true;
        std::vector<f32> trace;
        for (int i = 0; i < 300; ++i) {
            MoveCmd c{};
            // A varied but fixed schedule: run, turn, jump periodically, crouch.
            c.wish_dir = {std::sin(static_cast<f32>(i) * 0.1f), 0.0f,
                          std::cos(static_cast<f32>(i) * 0.1f)};
            c.jump = (i % 40) == 0;
            c.crouch = (i % 13) == 0;
            pm_move(s, c, t, kDt);
            if (s.grounded == false && (i % 50) == 0) s.grounded = true;  // "land"
            trace.push_back(s.velocity.x);
            trace.push_back(s.velocity.y);
            trace.push_back(s.velocity.z);
        }
        return trace;
    };
    REQUIRE(run() == run());
}
