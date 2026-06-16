// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/gameplay/Spread.cpp — see Spread.h.

#include "gameplay/Spread.h"

#include <cmath>  // std::sqrt only (correctly-rounded / deterministic per IEEE-754)

namespace psynder::gameplay {

u64 Rng::next_u64() noexcept {
    // splitmix64 — Steele/Lea/Flood. All operations are exact on u64, so the
    // sequence is bit-identical on every platform.
    state += 0x9E3779B97F4A7C15ull;
    u64 z = state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

f32 Rng::next_unit() noexcept {
    // Top 24 bits -> [0, 2^24) -> divide by 2^24. 24-bit ints are exactly
    // representable in f32, and 1/2^24 is exact, so the quotient is identical
    // across platforms. Result lies in [0, 1).
    const u32 bits24 = static_cast<u32>(next_u64() >> 40);  // 64 - 24 = 40
    return static_cast<f32>(bits24) * (1.0f / 16777216.0f);  // 2^24 = 16777216
}

f32 Rng::next_signed() noexcept {
    // [0,1) -> [-1,1). 2*u stays in [0,2); subtracting 1 gives [-1,1).
    return next_unit() * 2.0f - 1.0f;
}

u64 spread_seed(u32 tick, u32 entity_id, u32 shot_index) noexcept {
    // splitmix-style avalanche of a packed (tick, entity_id) word folded with
    // shot_index. Pure integer ops -> reproducible on every client.
    auto mix = [](u64 x) noexcept -> u64 {
        x += 0x9E3779B97F4A7C15ull;
        x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
        x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
        return x ^ (x >> 31);
    };
    const u64 hi = (static_cast<u64>(tick) << 32) | static_cast<u64>(entity_id);
    u64 h = mix(hi);
    h ^= mix(static_cast<u64>(shot_index) + 0x632BE59BD9B4E019ull);
    return mix(h);
}

math::Vec3 spread_direction(math::Vec3 base_dir_unit, f32 spread_tan,
                            u64 seed) noexcept {
    if (spread_tan <= 0.0f) return base_dir_unit;

    // Build an orthonormal basis (perp_x, perp_y) perpendicular to the base.
    // Pick a reference "up" not parallel to the base; if the base is near
    // vertical (|y| dominant), fall back to a world-X reference instead.
    const math::Vec3 up =
        (std::fabs(base_dir_unit.y) < 0.999f) ? math::Vec3{0.0f, 1.0f, 0.0f}
                                              : math::Vec3{1.0f, 0.0f, 0.0f};
    const math::Vec3 perp_x = math::normalize(math::cross(up, base_dir_unit));
    const math::Vec3 perp_y = math::cross(base_dir_unit, perp_x);  // already unit

    // Uniform sample inside the unit disk: angle is encoded by (cos,sin) drawn
    // as a random unit-ish vector (rejection-free, trig-free) and radius via
    // sqrt for area-uniformity. We get a 2D vector from two next_signed() draws
    // inside the unit square, then map to the disk by normalizing the direction
    // and scaling by sqrt(u) — no trig, just one sqrt.
    Rng rng(seed);
    f32 ox = rng.next_signed();
    f32 oy = rng.next_signed();
    f32 len2 = ox * ox + oy * oy;
    // Avoid a degenerate zero vector (would normalize to 0); nudge to +x.
    if (len2 <= 1.0e-12f) {
        ox = 1.0f;
        oy = 0.0f;
        len2 = 1.0f;
    }
    const f32 inv_len = 1.0f / std::sqrt(len2);
    // Unit-disk direction:
    const f32 dx = ox * inv_len;
    const f32 dy = oy * inv_len;
    // Area-uniform radius in [0,1): sqrt of a uniform draw.
    const f32 r = std::sqrt(rng.next_unit());
    const f32 sx = dx * r;  // disk-uniform 2D offset in [-1,1]^2 (|.| <= 1)
    const f32 sy = dy * r;

    // Offset within the cone: base + tan(half_angle) * (perp_x*sx + perp_y*sy).
    // Since the disk offset magnitude <= 1, the tangent of the resulting angle
    // is <= spread_tan, so the cone bound holds before normalization too.
    const math::Vec3 offset =
        math::add(math::mul(perp_x, sx * spread_tan), math::mul(perp_y, sy * spread_tan));
    return math::normalize(math::add(base_dir_unit, offset));
}

}  // namespace psynder::gameplay
