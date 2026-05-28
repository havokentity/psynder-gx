// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/physics/core/CharacterController.h
//
// The player's ECS representation. The player is an entity carrying:
//   * TransformWS        — translation column == capsule FOOT position (metres),
//                          matching the foot-origin convention of CharacterSpine.
//   * CharacterController — capsule params + gameplay-readable state
//                          (velocity, grounded) written back from the solver.
//   * CharacterInput     — desired move direction / speed / jump for this tick.
//
// The actual simulation is Jolt (CharacterVirtual via character_spine), bridged
// to this ECS state by EcsCharacterBridge.h: the ECS stays authoritative for the
// transform, Jolt is the solver. (An earlier revision hand-rolled an AABB
// resolver here; it was replaced by Jolt — see docs/adr/ADR-019 for the hybrid
// physics architecture: Jolt for the player + rigid/ragdoll/convex bodies, a
// custom DOTS system for homogeneous many-agent movement.)

#pragma once

#include "scene/SceneComponents.h"
#include "scene/World.h"

#include "math/Math.h"

#include "core/Types.h"

namespace psynder::physics {

// Player capsule state. Foot position lives in TransformWS (translation); the
// solver writes velocity/grounded back here for gameplay + debug overlays.
PSYNDER_COMPONENT(CharacterController) {
    f32 radius_m;            // capsule radius (metres)
    f32 height_m;            // total capsule height incl. hemispheres (metres)
    psynder::math::Vec3 velocity;  // world-space velocity (m/s), solver output
    f32 desired_vx;          // desired horizontal velocity X (m/s), input-driven
    f32 desired_vz;          // desired horizontal velocity Z (m/s)
    f32 jump_speed;          // launch vertical velocity when jumping (m/s)
    u32 jump_requested;      // 1 = jump edge requested (consumed on tick)
    u32 grounded;            // output: 1 = standing on a surface this tick
};

// Per-tick desired motion for a character (set by the player input layer).
PSYNDER_COMPONENT(CharacterInput) {
    f32 move_dir_x;   // desired move direction in world XZ (need not be unit)
    f32 move_dir_z;
    f32 speed_mps;    // target horizontal speed (m/s)
    u32 jump;         // 1 = request a jump this tick
};

// Spawn parameters for a player capsule entity.
struct CharacterDesc {
    psynder::math::Vec3 foot_position{0.0f, 0.0f, 0.0f};
    f32 radius_m   = 0.38f;
    f32 height_m   = 1.82f;
    f32 jump_speed = 4.5f;
};

inline psynder::math::Mat4 foot_transform(psynder::math::Vec3 foot) noexcept {
    return psynder::math::translate(foot);
}

// Create the player entity with the canonical capsule components. The position
// is driven each tick by the Jolt solver via EcsCharacterBridge.
inline Entity spawn_character(scene::World& world, const CharacterDesc& desc) {
    const Entity e = world.create();
    scene::TransformWS xf{};
    xf.mtw = foot_transform(desc.foot_position);
    xf.prev_mtw = xf.mtw;
    world.add(e, xf);
    CharacterController cc{};
    cc.radius_m = desc.radius_m;
    cc.height_m = desc.height_m;
    cc.velocity = {0.0f, 0.0f, 0.0f};
    cc.desired_vx = 0.0f;
    cc.desired_vz = 0.0f;
    cc.jump_speed = desc.jump_speed;
    cc.jump_requested = 0;
    cc.grounded = 0;
    world.add(e, cc);
    world.add(e, CharacterInput{0.0f, 0.0f, 0.0f, 0u});
    return e;
}

}  // namespace psynder::physics
