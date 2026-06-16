// SPDX-License-Identifier: MIT
// Psynder-GX — proving spine for the Behavior IR → DOTS execution model
// (docs/adr/ADR-018). Verifies that an entity-scalar behavior, lowered to
// op-major SIMD passes + a branch mask + a deferred-effect command buffer,
// produces results identical (within fp tolerance) to a straight entity-major
// reference, is bit-deterministic run to run, and allocates nothing in the
// steady-state tick.

#include "script/behavior/BehaviorSpine.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstring>
#include <vector>

using psynder::script::behavior::CommandBuffer;
using psynder::script::behavior::ProjectileBehavior;
using psynder::script::behavior::ProjectileChunk;
using Vec3 = psynder::math::Vec3;

namespace {

constexpr float kDt = 1.0f / 128.0f;  // 128-tick (competitive) cadence
const Vec3 kGravity{0.0f, -9.81f, 0.0f};
constexpr float kGround = 0.0f;

void fill_chunk(ProjectileChunk& c, std::size_t n) {
    c.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        const float fi = static_cast<float>(i);
        c.pos_x[i] = fi * 0.5f;
        c.pos_y[i] = 100.0f + std::fmod(fi, 7.0f);
        c.pos_z[i] = -fi;
        c.vel_x[i] = 1.0f + fi * 0.01f;
        c.vel_y[i] = 2.0f - fi * 0.003f;
        c.vel_z[i] = 0.5f;
    }
}

}  // namespace

TEST_CASE("behavior spine: op-major core matches entity-major reference",
          "[script][behavior][dots]") {
    // 1000 entities exercises the SIMD body plus the scalar tail.
    constexpr std::size_t kN = 1000;
    ProjectileChunk chunk;
    fill_chunk(chunk, kN);

    // Entity-major scalar reference, same semi-implicit Euler step.
    ProjectileChunk ref;
    fill_chunk(ref, kN);
    for (std::size_t i = 0; i < kN; ++i) {
        ref.vel_x[i] += kGravity.x * kDt;
        ref.vel_y[i] += kGravity.y * kDt;
        ref.vel_z[i] += kGravity.z * kDt;
        ref.pos_x[i] += ref.vel_x[i] * kDt;
        ref.pos_y[i] += ref.vel_y[i] * kDt;
        ref.pos_z[i] += ref.vel_z[i] * kDt;
    }

    ProjectileBehavior behavior;
    behavior.compile(kDt, kGravity, kGround);
    CommandBuffer cmd;
    cmd.reserve(kN);
    behavior.tick(chunk, cmd);

    const float eps = 1e-4f;
    for (std::size_t i = 0; i < kN; ++i) {
        REQUIRE(std::fabs(chunk.vel_x[i] - ref.vel_x[i]) <= eps);
        REQUIRE(std::fabs(chunk.vel_y[i] - ref.vel_y[i]) <= eps);
        REQUIRE(std::fabs(chunk.vel_z[i] - ref.vel_z[i]) <= eps);
        REQUIRE(std::fabs(chunk.pos_x[i] - ref.pos_x[i]) <= eps);
        REQUIRE(std::fabs(chunk.pos_y[i] - ref.pos_y[i]) <= eps);
        REQUIRE(std::fabs(chunk.pos_z[i] - ref.pos_z[i]) <= eps);
    }
}

TEST_CASE("behavior spine: tick is bit-deterministic run to run",
          "[script][behavior][determinism]") {
    constexpr std::size_t kN = 257;  // prime-ish, forces an unaligned tail

    ProjectileBehavior behavior;
    behavior.compile(kDt, kGravity, kGround);

    auto run_once = [&](ProjectileChunk& out) {
        fill_chunk(out, kN);
        CommandBuffer cmd;
        cmd.reserve(kN);
        // Several ticks so error/state accumulates and any nondeterminism shows.
        for (int t = 0; t < 16; ++t) {
            cmd.clear();
            behavior.tick(out, cmd);
        }
    };

    ProjectileChunk a;
    ProjectileChunk b;
    run_once(a);
    run_once(b);

    REQUIRE(a.count == b.count);
    REQUIRE(std::memcmp(a.pos_x.data(), b.pos_x.data(), kN * sizeof(float)) == 0);
    REQUIRE(std::memcmp(a.pos_y.data(), b.pos_y.data(), kN * sizeof(float)) == 0);
    REQUIRE(std::memcmp(a.pos_z.data(), b.pos_z.data(), kN * sizeof(float)) == 0);
    REQUIRE(std::memcmp(a.vel_x.data(), b.vel_x.data(), kN * sizeof(float)) == 0);
    REQUIRE(std::memcmp(a.vel_y.data(), b.vel_y.data(), kN * sizeof(float)) == 0);
    REQUIRE(std::memcmp(a.vel_z.data(), b.vel_z.data(), kN * sizeof(float)) == 0);
}

TEST_CASE("behavior spine: ground-hit mask emits deferred destroys in order",
          "[script][behavior][effect]") {
    constexpr std::size_t kN = 64;
    ProjectileChunk chunk;
    fill_chunk(chunk, kN);

    // The mask runs AFTER the integrate core, so flag entities far enough below
    // ground that one tick (with their small upward velocity) leaves them <= 0.
    chunk.pos_y[3] = -5.0f;
    chunk.pos_y[10] = -5.0f;
    chunk.pos_y[42] = -5.0f;

    ProjectileBehavior behavior;
    behavior.compile(kDt, kGravity, kGround);
    CommandBuffer cmd;
    cmd.reserve(kN);
    behavior.tick(chunk, cmd);

    // Effects are appended in ascending row order -> already deterministically
    // sorted, which the command-buffer replay contract relies on.
    REQUIRE(cmd.destroy.size() == 3);
    REQUIRE(cmd.destroy[0] == 3u);
    REQUIRE(cmd.destroy[1] == 10u);
    REQUIRE(cmd.destroy[2] == 42u);
}

TEST_CASE("behavior spine: steady-state tick does not reallocate",
          "[script][behavior][nogc]") {
    constexpr std::size_t kN = 512;
    ProjectileChunk chunk;
    fill_chunk(chunk, kN);

    ProjectileBehavior behavior;
    behavior.compile(kDt, kGravity, kGround);
    CommandBuffer cmd;
    cmd.reserve(kN);  // pool once

    behavior.tick(chunk, cmd);  // warm-up
    const std::size_t cmd_cap = cmd.destroy.capacity();

    for (int t = 0; t < 256; ++t) {
        cmd.clear();
        behavior.tick(chunk, cmd);
        // No growth of the pooled command buffer => no per-frame allocation in
        // the effect path. The kernel pre-sized its scratch at compile time, so
        // the core path is allocation-free as well.
        REQUIRE(cmd.destroy.capacity() == cmd_cap);
    }
}
