// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/gameplay/Spread.h — deterministic, lockstep-safe weapon spread.
//
// The deferred follow-up to the weapons system (engine/gameplay/Weapons.h): a
// seeded integer RNG plus a function that perturbs a fire direction within a
// cone WITHOUT any runtime trigonometry. libm sin/cos are NOT bit-identical
// across platforms/compilers, so calling them on the authoritative lockstep
// tick would desync clients. We instead:
//   * draw randomness from a pure-integer RNG (no float-RNG library), and
//   * build the cone with cross-product orthonormal-basis math + a single
//     std::sqrt (sqrt IS correctly-rounded / deterministic per IEEE-754).
//
// The cone half-angle is passed pre-converted as `spread_tan = tan(half_angle)`,
// computed OFFLINE so no tan/atan ever runs at simulation time. The returned
// direction is a unit vector and provably stays inside the cone:
//   dot(base_dir_unit, result) >= 1/sqrt(1 + spread_tan^2) == cos(half_angle).
//
// The gameplay lane is strict-FP and hot-path-linted: no std::shared_ptr /
// std::make_shared / dynamic_cast / typeid / throw appear here.

#pragma once

#include "math/Math.h"
#include "core/Types.h"

namespace psynder::gameplay {

// ─── Deterministic integer RNG (splitmix64) ───────────────────────────────
// A tiny, fast, fully deterministic generator. Same seed -> same sequence on
// every platform (pure 64-bit integer ops; no libm, no float-RNG). Floats are
// derived by dividing a 24-bit integer slice by 2^24, which is exactly
// representable in f32 and identical everywhere.
struct Rng {
    u64 state;

    explicit Rng(u64 seed) noexcept : state(seed) {}

    // Raw 64-bit splitmix64 output; advances the state.
    u64 next_u64() noexcept;

    // Uniform f32 in [0, 1). Uses the top 24 bits of next_u64() / 2^24.
    f32 next_unit() noexcept;

    // Uniform f32 in [-1, 1). Derived from next_unit().
    f32 next_signed() noexcept;
};

// Mix a (tick, entity_id, shot_index) triple into a u64 seed. Deterministic and
// lockstep-safe: identical inputs yield an identical seed on every client, so a
// shot fired on the same authoritative tick spreads identically for all peers.
u64 spread_seed(u32 tick, u32 entity_id, u32 shot_index) noexcept;

// Perturb `base_dir_unit` (assumed unit length) within a cone of half-angle
// atan(spread_tan), seeded by `seed`. Returns a unit vector. If spread_tan <= 0
// the base direction is returned unchanged. No runtime trigonometry: an
// orthonormal basis perpendicular to the base is built from cross products, a
// uniform unit-disk sample (one std::sqrt) is scaled by spread_tan, added to the
// base, and the sum is normalized. The result satisfies
//   dot(base_dir_unit, result) >= 1/sqrt(1 + spread_tan^2) == cos(half_angle).
math::Vec3 spread_direction(math::Vec3 base_dir_unit, f32 spread_tan,
                            u64 seed) noexcept;

}  // namespace psynder::gameplay
