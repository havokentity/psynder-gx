// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/gameplay_spread.cpp — deterministic, lockstep-safe weapon spread.

#include "gameplay/Spread.h"

#include "math/Math.h"
#include "core/Types.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cmath>

using namespace psynder;
using gameplay::Rng;
using gameplay::spread_direction;
using gameplay::spread_seed;
using math::Vec3;

namespace {
f32 vlen(Vec3 v) noexcept { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }
}  // namespace

TEST_CASE("spread: zero spread returns the base direction unchanged",
          "[gameplay][weapons]") {
    const Vec3 dir{0.0f, 0.0f, 1.0f};
    for (u32 i = 0; i < 8; ++i) {
        const Vec3 r = spread_direction(dir, 0.0f, spread_seed(i, 7, 0));
        REQUIRE(r.x == dir.x);
        REQUIRE(r.y == dir.y);
        REQUIRE(r.z == dir.z);
    }
    // Negative spread_tan is also a no-op.
    const Vec3 r = spread_direction(dir, -1.0f, 12345ull);
    REQUIRE(r.x == dir.x);
    REQUIRE(r.y == dir.y);
    REQUIRE(r.z == dir.z);
}

TEST_CASE("spread: results stay unit length and inside the cone",
          "[gameplay][weapons]") {
    const Vec3 base{1.0f, 0.0f, 0.0f};
    const f32 spread_tan = 0.1f;
    const f32 cos_half = 1.0f / std::sqrt(1.0f + spread_tan * spread_tan);

    for (u32 s = 0; s < 1000; ++s) {
        const Vec3 r = spread_direction(base, spread_tan, spread_seed(s, 3, s & 1u));
        REQUIRE(vlen(r) == Catch::Approx(1.0f).margin(1e-5f));
        const f32 d = math::dot(base, r);
        REQUIRE(d >= cos_half - 1e-4f);
    }
}

TEST_CASE("spread: works for a near-vertical base (up-vector fallback)",
          "[gameplay][weapons]") {
    const Vec3 base{0.0f, 1.0f, 0.0f};  // exercises the {1,0,0} fallback up-vector
    const f32 spread_tan = 0.25f;
    const f32 cos_half = 1.0f / std::sqrt(1.0f + spread_tan * spread_tan);

    for (u32 s = 0; s < 256; ++s) {
        const Vec3 r = spread_direction(base, spread_tan, spread_seed(s, 99, 0));
        REQUIRE(vlen(r) == Catch::Approx(1.0f).margin(1e-5f));
        REQUIRE(math::dot(base, r) >= cos_half - 1e-4f);
    }
}

TEST_CASE("spread: directions and seeds are reproducible (determinism)",
          "[gameplay][weapons][determinism]") {
    const Vec3 base{0.0f, 0.0f, 1.0f};
    const f32 spread_tan = 0.2f;

    // spread_seed is reproducible.
    REQUIRE(spread_seed(42, 13, 2) == spread_seed(42, 13, 2));

    // Same (base, tan, seed) -> identical vector on every call.
    const u64 seed = spread_seed(42, 13, 2);
    const Vec3 a = spread_direction(base, spread_tan, seed);
    const Vec3 b = spread_direction(base, spread_tan, seed);
    REQUIRE(a.x == b.x);
    REQUIRE(a.y == b.y);
    REQUIRE(a.z == b.z);

    // Different seeds generally give different directions: at least two of
    // several samples must differ.
    Vec3 samples[4];
    for (u32 i = 0; i < 4; ++i) {
        samples[i] = spread_direction(base, spread_tan, spread_seed(100 + i, 5, 0));
    }
    bool any_differ = false;
    for (u32 i = 1; i < 4 && !any_differ; ++i) {
        if (samples[i].x != samples[0].x || samples[i].y != samples[0].y ||
            samples[i].z != samples[0].z) {
            any_differ = true;
        }
    }
    REQUIRE(any_differ);

    // Distinct (tick, entity, shot) triples generally hash to distinct seeds.
    REQUIRE(spread_seed(1, 2, 3) != spread_seed(3, 2, 1));
}

TEST_CASE("spread: Rng sequence is in range and seed-deterministic",
          "[gameplay][weapons][determinism]") {
    // next_unit() stays in [0, 1).
    Rng r(0xDEADBEEFull);
    for (u32 i = 0; i < 2048; ++i) {
        const f32 u = r.next_unit();
        REQUIRE(u >= 0.0f);
        REQUIRE(u < 1.0f);
    }

    // next_signed() stays in [-1, 1).
    Rng rs(0x1234ull);
    for (u32 i = 0; i < 2048; ++i) {
        const f32 v = rs.next_signed();
        REQUIRE(v >= -1.0f);
        REQUIRE(v < 1.0f);
    }

    // Two RNGs with the same seed produce identical sequences.
    Rng x(987654321ull);
    Rng y(987654321ull);
    for (u32 i = 0; i < 64; ++i) {
        REQUIRE(x.next_u64() == y.next_u64());
    }

    // Different seeds diverge.
    Rng p(1ull);
    Rng q(2ull);
    bool differ = false;
    for (u32 i = 0; i < 16 && !differ; ++i) {
        if (p.next_u64() != q.next_u64()) differ = true;
    }
    REQUIRE(differ);
}
