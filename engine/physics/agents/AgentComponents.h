// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/physics/agents/AgentComponents.h
//
// DOTS mass-agent components (ADR-019 class 1).
//
// Thousands of homogeneous steering agents: cheap seek + local avoidance +
// capsule-vs-static push-out on the scene spatial broadphase. Agents are NOT
// full rigid bodies (Jolt owns the player + heavy props, ADR-019 class 2/3).
//
// Agents are ordinary ECS entities in the AUTHORITATIVE scene::World: each
// carries Agent + AgentVelocity + AgentTarget + TransformWS. Their world-space
// position lives in TransformWS (not duplicated here), so render extract,
// raycast, and netcode see them like any other entity — there is NO parallel
// mutable agent store (ADR-019: scene::World is the single authoritative scene).
//
// Coding rules (DESIGN-PSYNDER-GX.md §3 + AGENTS.md): all component types are
// trivially-copyable POD (the PSYNDER_COMPONENT macro static_asserts triviality);
// no ctors / vtables / shared_ptr / RTTI in the hot path. Real metric units:
// 1 world unit == 1 metre, velocities m/s, accelerations m/s^2, radii metres.

#pragma once

#include "math/Math.h"            // Vec3, translate
#include "scene/GxComponents.h"   // TransformWS
#include "scene/World.h"          // PSYNDER_COMPONENT, scene::World, Entity

namespace psynder::scene {

// Per-agent steering parameters. Set once on spawn; read-only during the tick.
//   max_speed_mps   — speed clamp, m/s (> 0)
//   max_force       — steering accel clamp, m/s^2 (> 0; agents are unit-mass
//                     kinematic movers, so "force" is acceleration here)
//   radius_m        — capsule radius, metres (separation + static push-out)
//   arrive_radius_m — Reynolds "arrive" braking radius, metres
PSYNDER_COMPONENT(Agent) {
    f32 max_speed_mps;
    f32 max_force;
    f32 radius_m;
    f32 arrive_radius_m;
};
static_assert(sizeof(Agent) == 16, "Agent layout frozen at 16 bytes");

// Current linear velocity (m/s, world space). Read AND written each tick.
PSYNDER_COMPONENT(AgentVelocity) {
    psynder::math::Vec3 velocity;
};
static_assert(sizeof(AgentVelocity) == 12, "AgentVelocity layout frozen at 12 bytes");

// World-space goal the agent seeks (metres). Written by gameplay/AI; read-only
// in the agent update.
PSYNDER_COMPONENT(AgentTarget) {
    psynder::math::Vec3 goal;
};
static_assert(sizeof(AgentTarget) == 12, "AgentTarget layout frozen at 12 bytes");

// Spawn a mass-agent entity into the authoritative scene with the canonical
// component set. Position seeds TransformWS (identity rotation, unit scale);
// velocity starts at rest. The agent system reads/writes its TransformWS.
inline Entity spawn_agent(World& world, const Agent& params,
                          psynder::math::Vec3 position,
                          psynder::math::Vec3 goal) {
    const Entity e = world.create();
    TransformWS t{};
    t.mtw = psynder::math::translate(position);
    t.prev_mtw = t.mtw;
    world.add(e, t);
    world.add(e, params);
    world.add(e, AgentVelocity{{0.0f, 0.0f, 0.0f}});
    world.add(e, AgentTarget{goal});
    return e;
}

}  // namespace psynder::scene
