// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/physics/core/CharacterSpine.h
//
// Lane 15 internal helper for server-authoritative character smoke scenes.
// This intentionally sits outside PublicPhysicsCore.h: the public contract is
// frozen, while samples/tests still need a small fixed-tick spine for crate /
// playable bring-up.

#pragma once

#include <cstdint>

namespace psynder::physics::character_spine {

struct World;
struct Character;

struct WorldDesc {
    float gravity_y_m_per_s2 = -9.81f;
    std::uint32_t max_bodies = 256;
    std::uint32_t tick_hz = 120;
};

struct BoxDesc {
    float center_m[3] = {0.0f, 0.0f, 0.0f};
    float rotation_quat[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    float half_extents_m[3] = {0.5f, 0.5f, 0.5f};
    float friction = 0.6f;
    float restitution = 0.0f;
};

struct SphereDesc {
    float center_m[3] = {0.0f, 0.0f, 0.0f};
    float rotation_quat[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    float radius_m = 0.5f;
    float friction = 0.6f;
    float restitution = 0.0f;
};

struct StaticCapsuleDesc {
    float center_m[3] = {0.0f, 0.0f, 0.0f};
    float rotation_quat[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    float radius_m = 0.40f;
    // Total capsule height in metres, including the two hemispheres.
    float height_m = 1.80f;
    float friction = 0.6f;
    float restitution = 0.0f;
};

struct CapsuleDesc {
    // Foot-origin position, matching CharacterVirtual and character_get_transform().
    float foot_position_m[3] = {0.0f, 0.1f, 0.0f};
    float radius_m = 0.40f;
    float height_m = 1.80f;
    float max_horizontal_speed_mps = 6.0f;
};

struct Transform {
    float foot_position_m[3] = {0.0f, 0.0f, 0.0f};
    float yaw_deg = 0.0f;
};

struct CharacterInput {
    // Desired horizontal velocity in world-space metres/second.
    float velocity_mps[2] = {0.0f, 0.0f};
    bool jump = false;
    bool crouch = false;
    bool prone = false;
    bool lean_left = false;
    bool lean_right = false;
};

// Opaque handle to a static body added via add_static_box / add_static_sphere.
// A default-constructed (zero) handle is invalid. Hand it to remove_static_body
// to delete that exact body — used to drop the Jolt collider of a destroyed prop
// (e.g. a shot crate) so no invisible wall is left behind. The id is stable: it
// is never reused for the life of the world, so a stale handle can't alias a
// later body.
struct StaticBodyHandle {
    std::uint64_t id = 0;
    constexpr bool valid() const noexcept { return id != 0; }
    constexpr explicit operator bool() const noexcept { return id != 0; }
    friend constexpr bool operator==(StaticBodyHandle,
                                     StaticBodyHandle) noexcept = default;
};

World* create_world(const WorldDesc& desc = {});
void   destroy_world(World* world);

// Adds a 1 m thick static ground slab whose top face is at top_y_m.
bool add_static_ground(World* world,
                       float half_extent_x_m,
                       float half_extent_z_m,
                       float top_y_m = 0.0f);
StaticBodyHandle add_static_box(World* world, const BoxDesc& desc);
StaticBodyHandle add_static_sphere(World* world, const SphereDesc& desc);
bool add_static_capsule(World* world, const StaticCapsuleDesc& desc);

// Removes a static body previously added to `world`. Returns false if the
// handle is invalid or the body is not present (e.g. already removed).
bool remove_static_body(World* world, StaticBodyHandle handle);

// ─── Dynamic rigid bodies (ADR-019 class 2) ────────────────────────────────
// Gravity-driven, stackable bodies that respond to impulses. Their solved
// transform is read back each tick (dynamic_body_transform) and written into
// the ECS. Opaque handle; same never-reused-id rule as StaticBodyHandle.
struct DynamicBodyHandle {
    std::uint64_t id = 0;
    constexpr bool valid() const noexcept { return id != 0; }
    constexpr explicit operator bool() const noexcept { return id != 0; }
    friend constexpr bool operator==(DynamicBodyHandle,
                                     DynamicBodyHandle) noexcept = default;
};

struct DynamicBoxDesc {
    float center_m[3] = {0.0f, 0.0f, 0.0f};
    float rotation_quat[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    float half_extents_m[3] = {0.5f, 0.5f, 0.5f};
    float mass_kg = 8.0f;
    float friction = 0.5f;
    float restitution = 0.1f;
};

struct DynamicSphereDesc {
    float center_m[3] = {0.0f, 0.0f, 0.0f};
    float rotation_quat[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    float radius_m = 0.5f;
    float mass_kg = 8.0f;
    float friction = 0.5f;
    float restitution = 0.1f;
};

DynamicBodyHandle add_dynamic_box(World* world, const DynamicBoxDesc& desc);
DynamicBodyHandle add_dynamic_sphere(World* world, const DynamicSphereDesc& desc);
bool remove_dynamic_body(World* world, DynamicBodyHandle handle);

// Reads a dynamic body's solved world transform: center position (metres) and
// rotation quaternion {x, y, z, w}. Returns false for an invalid handle.
bool dynamic_body_transform(const World* world, DynamicBodyHandle handle,
                            float out_pos[3], float out_quat[4]);

// Applies an instantaneous impulse (kg·m/s) consumed by the next step_fixed —
// used for shot knockback. No-op for an invalid handle.
void dynamic_body_apply_impulse(World* world, DynamicBodyHandle handle,
                                float ix, float iy, float iz);

Character* create_capsule_character(World* world, const CapsuleDesc& desc = {});
void       destroy_character(World* world, Character* character);

void set_character_input(Character* character, const CharacterInput& input);

// Desired horizontal velocity in world-space metres/second. It is clamped to
// CapsuleDesc::max_horizontal_speed_mps during step_fixed().
void set_desired_horizontal_velocity(Character* character,
                                     float vx_mps,
                                     float vz_mps);

void      step_fixed(World* world);
Transform character_transform(const Character* character);

} // namespace psynder::physics::character_spine
