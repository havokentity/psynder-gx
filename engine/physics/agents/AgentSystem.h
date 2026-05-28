// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/physics/agents/AgentSystem.h
//
// DOTS mass-agent steering system (ADR-019 class 1).
//
// Every fixed tick, for every agent entity in the authoritative scene::World:
//   1. SEEK toward AgentTarget (desired velocity clamped to max_speed, with a
//      Reynolds "arrive" brake inside arrive_radius_m),
//   2. LOCAL AVOIDANCE — separation from neighbours found via the scene spatial
//      broadphase (query_sphere within a perception radius),
//   3. cheap PUSH-OUT from static colliders (capsule-vs-AABB on the broadphase),
//   4. integrate (v += accel*dt clamped to max_speed; pos += v*dt) and write the
//      new world-space position back into the agent's TransformWS.
//
// DETERMINISM (hard pillar, 128-tick lockstep — ADR-019 / AGENTS.md):
//   * The update is split into a READ phase (snapshots last-tick positions and
//     reduces the separation/static accel serially) and a parallel COMPUTE+WRITE
//     phase, classic-boids style: jobs read only the immutable snapshot and
//     write only their own agent, so the result is independent of evaluation
//     order and bit-reproducible across thread counts.
//   * Agents are processed in a STABLE entity-id order, NOT ECS storage order
//     (the ECS uses swap-remove, so storage order is history-dependent).
//   * The lane is built -fno-fast-math -ffp-contract=off (see CMakeLists.txt).
//   * No per-frame heap allocation: all scratch lives in AgentScratch and is
//     reused across ticks (grown monotonically, never freed mid-sim).
//
// SYSTEM ORDER for lockstep: run AFTER gameplay/AI has written AgentTarget for
// this tick and AFTER Jolt has committed authoritative bodies into TransformWS,
// but BEFORE render extract reads TransformWS. Agents only read static colliders
// + other agents' last-tick positions, so within their pass they are a pure
// consumer of the prior tick's transforms.

#pragma once

#include <span>
#include <vector>

#include "core/Types.h"
#include "math/Math.h"
#include "physics/agents/AgentComponents.h"
#include "scene/GxComponents.h"  // TransformWS
#include "scene/Spatial.h"       // SpatialIndex
#include "scene/World.h"         // scene::World

namespace psynder::physics::agents {

// Steering tuning. Passed by value so a caller can tweak per sim without globals
// (determinism: identical params -> identical result). Metric defaults.
struct AgentTuning {
    f32 perception_radius_m = 3.0f;   ///< neighbour search radius for separation
    f32 separation_weight   = 1.6f;   ///< weight on the separation/push-out force
    f32 seek_weight         = 1.0f;   ///< weight on the seek force
    f32 static_skin_m       = 0.05f;  ///< extra clearance kept from static colliders
    u32 job_grain           = 64u;    ///< parallel_for chunk size (agents per job)
};

// The immovable colliders agents push out of, as parallel entity + AABB arrays
// (the shape SpatialIndex::rebuild expects). The system builds/refits its own
// index over these internally and reuses it across ticks.
struct StaticColliders {
    std::span<const Entity>     entities;  // stable ids
    std::span<const math::Aabb> aabbs;     // world-space boxes, metres
};

// Reused per-tick buffers (no per-frame heap alloc). Owned by the caller, one
// instance reused every tick; update_agents grows these monotonically when the
// agent/collider counts rise, then reuses the capacity. Holds the gathered agent
// SoA (snapshotted from the ECS), the id-sorted permutation, the spatial
// indices, and the cached component write-back pointers (valid within one tick;
// update_agents makes no structural ECS changes).
struct AgentScratch {
    // Gathered from the ECS each tick (index = gather order):
    std::vector<Entity>              entities;
    std::vector<scene::Agent>        params;
    std::vector<math::Vec3>          read_pos;   // last-tick positions (snapshot)
    std::vector<math::Vec3>          goal;
    std::vector<math::Vec3>          accel;      // separation + static accel
    std::vector<scene::TransformWS*> xf_ptr;     // write-back: TransformWS column
    std::vector<scene::AgentVelocity*> vel_ptr;  // read/write: velocity column

    // Deterministic ordering + broadphase scratch:
    std::vector<u32>        order;           // gather slots sorted by Entity id
    std::vector<math::Aabb> agent_aabbs;     // id-ordered agent AABBs
    std::vector<Entity>     agent_entities;  // id-ordered agent ids
    std::vector<Entity>     qhit;            // reused per-query neighbour buffer
    scene::SpatialIndex     agent_index;     // broadphase over the agents
    scene::SpatialIndex     static_index;    // broadphase over StaticColliders

    usize last_static_count = 0;
    u64   last_static_fingerprint = 0;
};

// Advance every agent entity in `ecs` by dt_seconds (the fixed-tick dt, e.g.
// 1/120). `statics` is the immovable environment. `scratch` MUST persist across
// ticks for determinism + zero-alloc. Deterministic: identical (world contents,
// statics, tuning, dt) inputs produce bit-identical TransformWS across runs and
// thread counts.
void update_agents(scene::World&          ecs,
                   const StaticColliders&  statics,
                   AgentScratch&           scratch,
                   f32                     dt_seconds,
                   const AgentTuning&      tuning = {});

// Convenience overload: no static environment (open field).
void update_agents(scene::World&     ecs,
                   AgentScratch&      scratch,
                   f32                dt_seconds,
                   const AgentTuning& tuning = {});

}  // namespace psynder::physics::agents
