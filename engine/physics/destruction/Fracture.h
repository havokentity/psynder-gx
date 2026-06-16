// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/physics/destruction/Fracture.h
//
// Lane 17 — deterministic precomputed-fracture core (ADR-021, issue #45).
//
// The full destruction feature spawns Jolt rigid-body shards on damage; this
// module is the DETERMINISTIC heart of it so the break replays bit-exact in
// lockstep netcode. We bake a FracturePattern (a grid split of a unit box into
// base shards), then fracture() applies SEEDED jitter to position and mass to
// produce the live shard set. Same seed in => identical shards out, on every
// platform — the whole reason this is a separate scaffold from the Jolt spawn.
//
// Determinism rules (DESIGN-PSYNDER-GX.md §3,§14 + AGENTS.md):
//   - Seeded inline xorshift64* RNG (NO std::random — its distributions are
//     non-deterministic across standard libraries).
//   - -fno-fast-math / -ffp-contract=off set by the lane CMakeLists.txt so the
//     float math is bit-reproducible.
//   - 1 world unit = 1 metre; real masses in kg; conservation is enforced.
//
// DOTS: Shard is POD / trivially-copyable, SoA-friendly fixed-size float
// arrays, no virtuals, no per-call heap alloc beyond the caller's out vector.

#pragma once

#include "core/Types.h"

#include <vector>

namespace psynder::physics::destruction {

// ─────────────────────────────────────────────────────────────────────────────
// Shard — one fractured piece. POD, trivially copyable, no padding surprises.
//
// An axis-aligned box approximation of a fragment: a centroid and half-extents
// in metres plus a mass in kilograms. The full M7 impl carries a mesh ref; the
// deterministic core only needs the rigid-body-relevant scalars.
// ─────────────────────────────────────────────────────────────────────────────
struct Shard {
    f32 centroid_m[3];      // centre of mass in asset-local space (metres)
    f32 half_extents_m[3];  // half-size of the AABB approximation (metres)
    f32 mass_kg;            // mass in kilograms (> 0)
};
static_assert(sizeof(Shard) == 28, "Shard must stay tightly packed POD");
static_assert(std::is_trivially_copyable_v<Shard>, "Shard must be POD for DOTS");

// ─────────────────────────────────────────────────────────────────────────────
// FracturePattern — a baked set of base shards for a source box.
//
// Built once (offline / on load) from a deterministic grid split of a box of
// the given full extents and total mass. fracture() then perturbs a COPY of
// these base shards with seeded jitter. The pattern itself is seed-independent
// so a single bake can drive many seeds.
// ─────────────────────────────────────────────────────────────────────────────
struct FracturePattern {
    f32                source_extents_m[3];  // full size of the source box (m)
    f32                source_mass_kg;       // total mass of the source (kg)
    std::vector<Shard> base_shards;          // unperturbed grid cells

    // Sum of base_shards masses; equals source_mass_kg by construction. Kept so
    // fracture() can renormalise after mass jitter to conserve total mass.
    f32 total_base_mass_kg() const noexcept;
};

// Bake a grid-split pattern: divisions[i] cells along axis i (each >= 1). Mass
// is distributed by cell volume (uniform density), positions are the cell
// centres. Fully deterministic — no RNG here.
FracturePattern make_grid_pattern(const f32 source_extents_m[3],
                                  f32 source_mass_kg,
                                  const u32 divisions[3]);

// Convenience: a unit (1 m cube), 1 kg, split into a 2x2x2 grid (8 shards).
FracturePattern make_unit_box_pattern();

// ─────────────────────────────────────────────────────────────────────────────
// fracture — produce the live shard set from a pattern + seed.
//
// Applies deterministic seeded jitter to each base shard's centroid (within a
// fraction of its half-extents) and mass, then renormalises masses so the
// total exactly conserves pattern.source_mass_kg. `out` is cleared then filled;
// the same (pattern, seed) always yields a bit-identical `out`.
// ─────────────────────────────────────────────────────────────────────────────
void fracture(const FracturePattern& pattern, u64 seed, std::vector<Shard>& out);

// ─────────────────────────────────────────────────────────────────────────────
// PlacedShard — a shard positioned in world space (centroid + half-extents in
// metres, mass in kg). The bridge that spawns Jolt rigid bodies / ECS props
// consumes these; this struct deliberately stays free of math-lib types so the
// destruction lane has no dependency on scene/physics-core.
// ─────────────────────────────────────────────────────────────────────────────
struct PlacedShard {
    f32 center_m[3];        // world-space centroid (metres)
    f32 half_extents_m[3];  // AABB half-size (metres)
    f32 mass_kg;            // mass in kilograms (> 0)
};
static_assert(sizeof(PlacedShard) == 28, "PlacedShard must stay tightly packed POD");

// Transform asset-local shards into world space given the source body's world
// centre and rotation quaternion {x, y, z, w}. Each local centroid is rotated
// by the quaternion (Rodrigues form, no matrix/library dependence) and offset
// by the centre; half-extents and mass pass through unchanged. Pure and
// deterministic — identical inputs yield bit-identical output on every target,
// so a fractured break replays bit-exact in lockstep (ADR-021/#45).
void place_shards(const std::vector<Shard>& local,
                  const f32 center_m[3],
                  const f32 rotation_quat[4],
                  std::vector<PlacedShard>& out);

} // namespace psynder::physics::destruction
