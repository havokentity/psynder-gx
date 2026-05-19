// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/physics/core/PublicPhysicsCore.h
//
// Lane 15 — Physics core public-header CONTRACT (Wave 0).
//
// Jolt-backed rigid bodies, fixed 120 Hz tick, character controller.
// Deterministic across platforms (precondition for lockstep replay).
// Real metric units throughout: 1 unit = 1 metre, kg / N / N·m, gravity
// 9.81 m/s².
//
// See DESIGN-PSYNDER-GX.md §10.1.

#pragma once

#include <cstdint>

namespace psynder::physics {

struct World;
struct RigidBody;
struct CharacterController;

struct WorldDesc {
    float gravity_y_m_per_s2 = -9.81f;
    std::uint32_t max_bodies = 64'000;
    std::uint32_t max_constraints = 64'000;
    std::uint32_t tick_hz = 120;
};

World* create_world(const WorldDesc&);
void   destroy_world(World*);
void   tick(World*, float dt_seconds); // typically called at fixed dt = 1/120

// ─── Rigid bodies ───────────────────────────────────────────────────────
enum class Shape : std::uint8_t {
    Box, Sphere, Capsule, ConvexHull, TriangleMesh,
};

struct BodyDesc {
    Shape shape;
    float pos[3];
    float rot_quat[4]; // {x, y, z, w}
    float mass_kg = 0.0f; // 0 = static
    float friction = 0.5f;
    float restitution = 0.1f;
    // Shape-specific dimensions (extents for box, radius for sphere, etc.)
    float dims[4] = {};
};
RigidBody* create_body(World*, const BodyDesc&);
void       destroy_body(World*, RigidBody*);
void       body_apply_force(RigidBody*, float fx, float fy, float fz);
void       body_get_transform(const RigidBody*, float out_pos[3], float out_quat[4]);

// ─── Character controller (FPS capsule, DESIGN §10.1) ───────────────────
enum class CharacterStance : std::uint8_t { Stand, Crouch, Prone };

struct CharacterDesc {
    float pos[3];
    float capsule_radius_m = 0.40f;
    float capsule_height_m = 1.80f;
    float walk_speed_mps   = 5.0f;
    float run_speed_mps    = 8.0f;
    float jump_vel_mps     = 4.5f;
};
CharacterController* create_character(World*, const CharacterDesc&);
void                 destroy_character(World*, CharacterController*);

struct CharacterInput {
    float move_xy[2];        // [-1..1]
    bool  jump = false;
    bool  crouch = false;
    bool  prone = false;
    bool  lean_left = false;
    bool  lean_right = false;
};
void character_tick(CharacterController*, const CharacterInput&, float dt);
void character_get_transform(const CharacterController*, float out_pos[3], float out_yaw_deg);

} // namespace psynder::physics
