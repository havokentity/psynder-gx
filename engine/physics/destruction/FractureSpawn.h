// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/physics/destruction/FractureSpawn.h
//
// Lane 17 — bridge the deterministic fracture core (Fracture.h) into a live
// scene: turn a shot static prop into a burst of Jolt dynamic shards (ADR-021,
// issue #45; ADR-019 class 2). This is the glue the TODO in Fracture.h called
// out: pattern -> seeded fracture -> world placement -> ECS dynamic props + Jolt
// rigid bodies -> launch impulse.
//
// It lives in the destruction lane (which already links scene + physics_core) so
// it is unit-testable headlessly — the determinism-critical shard geometry comes
// from Fracture.cpp (built with -fno-fast-math), and the spawn/launch is a thin,
// deterministic layer on top.

#pragma once

#include "physics/destruction/Fracture.h"

#include "physics/core/CharacterSpine.h"
#include "physics/core/EcsCharacterBridge.h"

#include "scene/SceneComponents.h"
#include "scene/World.h"

#include "math/Math.h"
#include "core/Types.h"

#include <vector>

namespace psynder::physics::destruction {

// Launch tuning for the shard burst. Impulses are mass-scaled so every shard
// leaves at the same speed regardless of its (jittered) mass.
struct ShardSpawnParams {
    f32 source_mass_kg = 8.0f;      // total source mass, distributed across shards
    u32 divisions[3] = {2u, 2u, 2u};
    f32 burst_mps = 3.2f;           // outward separation speed from the centre
    f32 shot_mps = 2.4f;            // shove along the shot direction
    f32 pop_mps = 2.0f;             // upward pop
};

// Fracture a source body (its TransformWS + Collider) into dynamic shards spawned
// directly into the ECS (as DynamicProps) and the Jolt world (as rigid bodies),
// each launched outward from the source centre plus along `shot_dir`. The shards
// inherit `look`. Deterministic in `seed` (same seed -> identical break), so a
// recorded shot reproduces under replay (#38). Spawned entity->handle pairs are
// appended to `out_bodies` so the caller's per-tick sync keeps driving them.
// Returns the number of shards actually spawned (fewer if the Jolt body pool is
// exhausted — orphan ECS entities are rolled back in that case).
u32 spawn_fracture_shards(character_spine::World* phys,
                          scene::World& ecs,
                          const scene::TransformWS& source_xf,
                          const scene::Collider& source_col,
                          const scene::RenderMaterial& look,
                          const math::Vec3& shot_dir,
                          u64 seed,
                          std::vector<EcsDynamicBody>& out_bodies,
                          const ShardSpawnParams& params = {});

}  // namespace psynder::physics::destruction
