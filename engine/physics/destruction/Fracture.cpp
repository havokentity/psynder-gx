// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/physics/destruction/Fracture.cpp
//
// Lane 17 — deterministic precomputed-fracture core (ADR-021, issue #45).
// See Fracture.h for the contract and the determinism rationale.

#include "physics/destruction/Fracture.h"

#include <cstdint>

namespace psynder::physics::destruction {

namespace {

// ─────────────────────────────────────────────────────────────────────────────
// xorshift64* — a tiny, fully specified, platform-independent PRNG.
//
// Chosen over std::random because the standard distribution engines are NOT
// reproducible across standard-library implementations (libc++ vs libstdc++ vs
// MSVC STL all differ), which would break lockstep determinism. xorshift64* has
// a fixed integer algorithm with no floating-point or library dependence, so it
// is bit-identical everywhere. Reference: Vigna, "An experimental exploration
// of Marsaglia's xorshift generators, scrambled" (2014).
// ─────────────────────────────────────────────────────────────────────────────
struct Xorshift64 {
    u64 state;

    explicit Xorshift64(u64 seed) noexcept
        // Avoid the all-zero fixed point: splitmix the seed once so any input
        // (including 0) maps to a usable nonzero state.
        : state(seed ? seed : 0x9E3779B97F4A7C15ull)
    {
        // One splitmix64 step to decorrelate low-entropy seeds.
        u64 z = (state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        state = z ^ (z >> 31);
        if (state == 0) state = 0x9E3779B97F4A7C15ull;
    }

    u64 next_u64() noexcept {
        u64 x = state;
        x ^= x >> 12;
        x ^= x << 25;
        x ^= x >> 27;
        state = x;
        return x * 0x2545F4914F6CDD1Dull;
    }

    // Uniform f32 in [-1, 1). Built from the top 24 bits so the mantissa is
    // exactly representable -> identical bits on every IEEE-754 target.
    f32 next_signed_unit() noexcept {
        const u32 bits24 = static_cast<u32>(next_u64() >> 40);  // top 24 bits
        const f32 unit01 = static_cast<f32>(bits24) * (1.0f / 16777216.0f); // /2^24
        return unit01 * 2.0f - 1.0f;
    }
};

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
f32 FracturePattern::total_base_mass_kg() const noexcept {
    f32 sum = 0.0f;
    for (const Shard& s : base_shards) sum += s.mass_kg;
    return sum;
}

// ─────────────────────────────────────────────────────────────────────────────
FracturePattern make_grid_pattern(const f32 source_extents_m[3],
                                  f32 source_mass_kg,
                                  const u32 divisions[3]) {
    FracturePattern p{};
    p.source_extents_m[0] = source_extents_m[0];
    p.source_extents_m[1] = source_extents_m[1];
    p.source_extents_m[2] = source_extents_m[2];
    p.source_mass_kg      = source_mass_kg;

    u32 dx = divisions[0] ? divisions[0] : 1u;
    u32 dy = divisions[1] ? divisions[1] : 1u;
    u32 dz = divisions[2] ? divisions[2] : 1u;

    // Uniform density: every cell has identical volume, so identical mass. The
    // cell mass is total / cell_count, which conserves mass exactly modulo the
    // single float division (the test asserts conservation within tolerance).
    const u32 cell_count = dx * dy * dz;
    const f32 cell_mass  = source_mass_kg / static_cast<f32>(cell_count);

    const f32 cell_full[3] = {
        source_extents_m[0] / static_cast<f32>(dx),
        source_extents_m[1] / static_cast<f32>(dy),
        source_extents_m[2] / static_cast<f32>(dz),
    };
    const f32 origin[3] = {
        -0.5f * source_extents_m[0],
        -0.5f * source_extents_m[1],
        -0.5f * source_extents_m[2],
    };

    p.base_shards.reserve(cell_count);
    // Iterate in a fixed (z,y,x) order so the shard sequence is deterministic.
    for (u32 iz = 0; iz < dz; ++iz) {
        for (u32 iy = 0; iy < dy; ++iy) {
            for (u32 ix = 0; ix < dx; ++ix) {
                Shard s{};
                s.centroid_m[0] = origin[0] + (static_cast<f32>(ix) + 0.5f) * cell_full[0];
                s.centroid_m[1] = origin[1] + (static_cast<f32>(iy) + 0.5f) * cell_full[1];
                s.centroid_m[2] = origin[2] + (static_cast<f32>(iz) + 0.5f) * cell_full[2];
                s.half_extents_m[0] = 0.5f * cell_full[0];
                s.half_extents_m[1] = 0.5f * cell_full[1];
                s.half_extents_m[2] = 0.5f * cell_full[2];
                s.mass_kg = cell_mass;
                p.base_shards.push_back(s);
            }
        }
    }
    return p;
}

// ─────────────────────────────────────────────────────────────────────────────
FracturePattern make_unit_box_pattern() {
    const f32 extents[3] = {1.0f, 1.0f, 1.0f};  // 1 m cube
    const u32 divs[3]    = {2u, 2u, 2u};         // 8 shards
    return make_grid_pattern(extents, /*source_mass_kg=*/1.0f, divs);
}

// ─────────────────────────────────────────────────────────────────────────────
void fracture(const FracturePattern& pattern, u64 seed, std::vector<Shard>& out) {
    out.clear();
    out.reserve(pattern.base_shards.size());

    Xorshift64 rng(seed);

    // Position jitter is a fraction of each shard's half-extent so a shard
    // never wanders out of its own cell footprint. Mass jitter is a fraction of
    // the base mass; we renormalise afterwards to conserve the total exactly.
    constexpr f32 kPosJitterFrac  = 0.25f;
    constexpr f32 kMassJitterFrac = 0.30f;

    f32 jittered_mass_sum = 0.0f;
    for (const Shard& base : pattern.base_shards) {
        Shard s = base;
        s.centroid_m[0] += rng.next_signed_unit() * kPosJitterFrac * base.half_extents_m[0];
        s.centroid_m[1] += rng.next_signed_unit() * kPosJitterFrac * base.half_extents_m[1];
        s.centroid_m[2] += rng.next_signed_unit() * kPosJitterFrac * base.half_extents_m[2];

        const f32 mass_scale = 1.0f + rng.next_signed_unit() * kMassJitterFrac;
        s.mass_kg = base.mass_kg * mass_scale;
        jittered_mass_sum += s.mass_kg;

        out.push_back(s);
    }

    // Conserve total mass: renormalise so sum(out.mass_kg) == source_mass_kg.
    // Guard against a degenerate zero sum (cannot happen for positive masses,
    // but keeps the math total).
    if (jittered_mass_sum > 0.0f) {
        const f32 norm = pattern.source_mass_kg / jittered_mass_sum;
        for (Shard& s : out) s.mass_kg *= norm;
    }
}

// TODO(ADR-021/#45): spawn shards as Jolt rigid bodies (ADR-019 class 2) via
// character_spine; route through DestructionWorldState (#1).

} // namespace psynder::physics::destruction
