// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/physics_lagcomp.cpp
//
// Lane 15 (physics-core) — ADR-020 / issue #42: lag-compensation rewind.
//
// Proves the HitboxHistory ring buffer records per-tick hitbox poses and that
// rewind() returns a PAST pose (the closest recorded tick <= the requested
// tick), not the latest. The second case is the whole point of lag comp: a
// ray fired at where a target WAS hits, while the same ray against where the
// target IS now misses.

#include "physics/core/HitboxHistory.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>

using namespace psynder;
using psynder::physics::HitboxHistory;
using psynder::physics::HitboxPose;

namespace {

// Local slab test (ray vs world-space AABB), mirroring the pattern in
// samples/combat/Combat.h::ray_aabb. Intentionally self-contained so the test
// does not depend on the sample. Returns true if the ray enters the box within
// [0, max_t]; out_t receives the entry distance along `dir`.
bool ray_aabb(const f32 origin[3], const f32 dir[3], const f32 lo[3],
              const f32 hi[3], f32 max_t, f32& out_t) {
    f32 tmin = 0.0f;
    f32 tmax = max_t;
    for (int axis = 0; axis < 3; ++axis) {
        if (std::fabs(dir[axis]) < 1e-8f) {
            if (origin[axis] < lo[axis] || origin[axis] > hi[axis]) return false;
            continue;
        }
        const f32 inv = 1.0f / dir[axis];
        f32 t1 = (lo[axis] - origin[axis]) * inv;
        f32 t2 = (hi[axis] - origin[axis]) * inv;
        if (t1 > t2) { const f32 s = t1; t1 = t2; t2 = s; }
        tmin = t1 > tmin ? t1 : tmin;
        tmax = t2 < tmax ? t2 : tmax;
        if (tmin > tmax) return false;
    }
    out_t = tmin;
    return true;
}

// Convenience: ray vs a single HitboxPose (centre + half-extents -> AABB).
bool ray_hits_pose(const f32 origin[3], const f32 dir[3], const HitboxPose& p,
                   f32 max_t) {
    const f32 lo[3] = {p.center[0] - p.half_extents[0],
                       p.center[1] - p.half_extents[1],
                       p.center[2] - p.half_extents[2]};
    const f32 hi[3] = {p.center[0] + p.half_extents[0],
                       p.center[1] + p.half_extents[1],
                       p.center[2] + p.half_extents[2]};
    f32 t = 0.0f;
    return ray_aabb(origin, dir, lo, hi, max_t, t);
}

}  // namespace

TEST_CASE("lagcomp: rewind returns the past pose, not the latest",
          "[physics][lagcomp][adr020]") {
    HitboxHistory<128, 8> history;

    // A single 1x1x1 m hitbox slides along +X, one metre per tick, 64 ticks.
    constexpr u32 kTicks = 64;
    for (u32 tick = 0; tick < kTicks; ++tick) {
        HitboxPose pose{};
        pose.center[0] = static_cast<f32>(tick);  // x = tick metres
        pose.center[1] = 0.0f;
        pose.center[2] = 0.0f;
        pose.half_extents[0] = 0.5f;
        pose.half_extents[1] = 0.5f;
        pose.half_extents[2] = 0.5f;
        const std::array<HitboxPose, 1> frame{pose};
        history.record(tick, std::span<const HitboxPose>(frame.data(), 1));
    }

    REQUIRE(history.size() == kTicks);

    // Rewind to tick 10 — must report x == 10, NOT the latest (x == 63).
    constexpr u32 kPastTick = 10;
    std::span<const HitboxPose> past = history.rewind(kPastTick);
    REQUIRE(past.size() == 1);
    REQUIRE(past[0].center[0] == static_cast<f32>(kPastTick));

    // Latest tick reports the most recent pose.
    std::span<const HitboxPose> latest = history.rewind(kTicks - 1);
    REQUIRE(latest.size() == 1);
    REQUIRE(latest[0].center[0] == static_cast<f32>(kTicks - 1));

    // Closest-at-or-before semantics: a tick with no exact record falls back
    // to the newest earlier tick (every tick was recorded here, so an
    // out-of-range future tick clamps to the latest).
    std::span<const HitboxPose> future = history.rewind(kTicks + 100);
    REQUIRE(future.size() == 1);
    REQUIRE(future[0].center[0] == static_cast<f32>(kTicks - 1));

    // ── The point of lag comp: a ray aimed at the PAST position hits, while
    //    the same ray against the PRESENT position misses. ──
    // Ray straight down (-Y) through x == kPastTick, well above the box.
    const f32 origin[3] = {static_cast<f32>(kPastTick), 5.0f, 0.0f};
    const f32 dir[3]    = {0.0f, -1.0f, 0.0f};
    const f32 kMaxT     = 100.0f;

    REQUIRE(ray_hits_pose(origin, dir, past[0], kMaxT));      // rewound -> HIT
    REQUIRE_FALSE(ray_hits_pose(origin, dir, latest[0], kMaxT));  // latest -> MISS
}

TEST_CASE("lagcomp: empty history and pre-history rewind yield empty spans",
          "[physics][lagcomp][adr020]") {
    HitboxHistory<32, 4> history;
    REQUIRE(history.empty());
    REQUIRE(history.rewind(0).empty());

    HitboxPose pose{};
    pose.center[0] = 7.0f;
    const std::array<HitboxPose, 1> frame{pose};
    history.record(50, std::span<const HitboxPose>(frame.data(), 1));

    // Requesting a tick BEFORE the first recorded tick: nothing at/<= it.
    REQUIRE(history.rewind(49).empty());
    // At/after the recorded tick: the pose is returned.
    REQUIRE(history.rewind(50).size() == 1);
    REQUIRE(history.rewind(99)[0].center[0] == 7.0f);
}
