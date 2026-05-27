// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/physics/core/CharacterSpine.cpp

#include "physics/core/CharacterSpine.h"

#include "physics/core/PublicPhysicsCore.h"

#include <algorithm>
#include <cmath>
#include <new>
#include <vector>

namespace psynder::physics::character_spine {

namespace {

float non_negative(float v, float fallback) {
    return v > 0.0f ? v : fallback;
}

} // namespace

struct Character {
    ::psynder::physics::CharacterController* controller = nullptr;
    World* owner = nullptr;
    CharacterInput input{};
    float max_horizontal_speed_mps = 6.0f;
};

struct World {
    ::psynder::physics::World* physics_world = nullptr;
    std::uint32_t tick_hz = 120;
    float fixed_dt_seconds = 1.0f / 120.0f;
    std::vector<::psynder::physics::RigidBody*> static_bodies;
    std::vector<Character*> characters;
};

World* create_world(const WorldDesc& desc) {
    auto* world = new (std::nothrow) World();
    if (!world) return nullptr;

    ::psynder::physics::WorldDesc physics_desc{};
    physics_desc.gravity_y_m_per_s2 = desc.gravity_y_m_per_s2;
    physics_desc.max_bodies = desc.max_bodies > 0 ? desc.max_bodies : 256u;
    physics_desc.max_constraints = physics_desc.max_bodies;
    physics_desc.tick_hz = desc.tick_hz > 0 ? desc.tick_hz : 120u;

    world->physics_world = ::psynder::physics::create_world(physics_desc);
    if (!world->physics_world) {
        delete world;
        return nullptr;
    }

    world->tick_hz = physics_desc.tick_hz;
    world->fixed_dt_seconds = 1.0f / static_cast<float>(world->tick_hz);
    return world;
}

void destroy_world(World* world) {
    if (!world) return;

    for (Character* character : world->characters) {
        if (character) {
            ::psynder::physics::destroy_character(world->physics_world,
                                                  character->controller);
            delete character;
        }
    }
    world->characters.clear();

    for (::psynder::physics::RigidBody* body : world->static_bodies) {
        ::psynder::physics::destroy_body(world->physics_world, body);
    }
    world->static_bodies.clear();

    ::psynder::physics::destroy_world(world->physics_world);
    delete world;
}

bool add_static_box(World* world, const BoxDesc& desc) {
    if (!world || !world->physics_world) return false;
    if (desc.half_extents_m[0] <= 0.0f ||
        desc.half_extents_m[1] <= 0.0f ||
        desc.half_extents_m[2] <= 0.0f) {
        return false;
    }

    ::psynder::physics::BodyDesc body{};
    body.shape = ::psynder::physics::Shape::Box;
    body.pos[0] = desc.center_m[0];
    body.pos[1] = desc.center_m[1];
    body.pos[2] = desc.center_m[2];
    body.rot_quat[0] = desc.rotation_quat[0];
    body.rot_quat[1] = desc.rotation_quat[1];
    body.rot_quat[2] = desc.rotation_quat[2];
    body.rot_quat[3] = desc.rotation_quat[3];
    body.mass_kg = 0.0f;
    body.friction = desc.friction;
    body.restitution = desc.restitution;
    body.dims[0] = desc.half_extents_m[0];
    body.dims[1] = desc.half_extents_m[1];
    body.dims[2] = desc.half_extents_m[2];

    ::psynder::physics::RigidBody* rigid_body =
        ::psynder::physics::create_body(world->physics_world, body);
    if (!rigid_body) return false;

    world->static_bodies.push_back(rigid_body);
    return true;
}

bool add_static_sphere(World* world, const SphereDesc& desc) {
    if (!world || !world->physics_world) return false;
    if (desc.radius_m <= 0.0f) return false;

    ::psynder::physics::BodyDesc body{};
    body.shape = ::psynder::physics::Shape::Sphere;
    body.pos[0] = desc.center_m[0];
    body.pos[1] = desc.center_m[1];
    body.pos[2] = desc.center_m[2];
    body.rot_quat[0] = desc.rotation_quat[0];
    body.rot_quat[1] = desc.rotation_quat[1];
    body.rot_quat[2] = desc.rotation_quat[2];
    body.rot_quat[3] = desc.rotation_quat[3];
    body.mass_kg = 0.0f;
    body.friction = desc.friction;
    body.restitution = desc.restitution;
    body.dims[0] = desc.radius_m;

    ::psynder::physics::RigidBody* rigid_body =
        ::psynder::physics::create_body(world->physics_world, body);
    if (!rigid_body) return false;

    world->static_bodies.push_back(rigid_body);
    return true;
}

bool add_static_capsule(World* world, const StaticCapsuleDesc& desc) {
    if (!world || !world->physics_world) return false;
    if (desc.radius_m <= 0.0f || desc.height_m <= 0.0f) return false;

    const float half_segment = (desc.height_m - 2.0f * desc.radius_m) * 0.5f;
    if (half_segment <= 0.0f) return false;

    ::psynder::physics::BodyDesc body{};
    body.shape = ::psynder::physics::Shape::Capsule;
    body.pos[0] = desc.center_m[0];
    body.pos[1] = desc.center_m[1];
    body.pos[2] = desc.center_m[2];
    body.rot_quat[0] = desc.rotation_quat[0];
    body.rot_quat[1] = desc.rotation_quat[1];
    body.rot_quat[2] = desc.rotation_quat[2];
    body.rot_quat[3] = desc.rotation_quat[3];
    body.mass_kg = 0.0f;
    body.friction = desc.friction;
    body.restitution = desc.restitution;
    body.dims[0] = desc.radius_m;
    body.dims[1] = half_segment;

    ::psynder::physics::RigidBody* rigid_body =
        ::psynder::physics::create_body(world->physics_world, body);
    if (!rigid_body) return false;

    world->static_bodies.push_back(rigid_body);
    return true;
}

bool add_static_ground(World* world,
                       float half_extent_x_m,
                       float half_extent_z_m,
                       float top_y_m) {
    BoxDesc ground{};
    ground.center_m[0] = 0.0f;
    ground.center_m[1] = top_y_m - 0.5f;
    ground.center_m[2] = 0.0f;
    ground.half_extents_m[0] = half_extent_x_m;
    ground.half_extents_m[1] = 0.5f;
    ground.half_extents_m[2] = half_extent_z_m;
    ground.friction = 0.8f;
    return add_static_box(world, ground);
}

Character* create_capsule_character(World* world, const CapsuleDesc& desc) {
    if (!world || !world->physics_world) return nullptr;

    const float max_speed =
        non_negative(desc.max_horizontal_speed_mps, 6.0f);

    ::psynder::physics::CharacterDesc character_desc{};
    character_desc.pos[0] = desc.foot_position_m[0];
    character_desc.pos[1] = desc.foot_position_m[1];
    character_desc.pos[2] = desc.foot_position_m[2];
    character_desc.capsule_radius_m = non_negative(desc.radius_m, 0.40f);
    character_desc.capsule_height_m = non_negative(desc.height_m, 1.80f);
    character_desc.walk_speed_mps = max_speed;
    character_desc.run_speed_mps = max_speed;

    auto* controller =
        ::psynder::physics::create_character(world->physics_world,
                                             character_desc);
    if (!controller) return nullptr;

    auto* character = new (std::nothrow) Character();
    if (!character) {
        ::psynder::physics::destroy_character(world->physics_world, controller);
        return nullptr;
    }

    character->controller = controller;
    character->owner = world;
    character->max_horizontal_speed_mps = max_speed;
    world->characters.push_back(character);
    return character;
}

void destroy_character(World* world, Character* character) {
    if (!world || !character) return;

    auto it = std::find(world->characters.begin(),
                        world->characters.end(),
                        character);
    if (it != world->characters.end()) {
        world->characters.erase(it);
    }

    ::psynder::physics::destroy_character(world->physics_world,
                                          character->controller);
    delete character;
}

void set_character_input(Character* character, const CharacterInput& input) {
    if (!character) return;
    character->input = input;
}

void set_desired_horizontal_velocity(Character* character,
                                     float vx_mps,
                                     float vz_mps) {
    if (!character) return;
    character->input.velocity_mps[0] = vx_mps;
    character->input.velocity_mps[1] = vz_mps;
}

void step_fixed(World* world) {
    if (!world || !world->physics_world) return;

    for (Character* character : world->characters) {
        if (!character || !character->controller) continue;

        float vx = character->input.velocity_mps[0];
        float vz = character->input.velocity_mps[1];
        const float max_speed = character->max_horizontal_speed_mps;
        const float speed_sq = vx * vx + vz * vz;
        if (max_speed > 0.0f && speed_sq > max_speed * max_speed) {
            const float scale = max_speed / std::sqrt(speed_sq);
            vx *= scale;
            vz *= scale;
        }

        ::psynder::physics::CharacterInput input{};
        if (max_speed > 0.0f) {
            input.move_xy[0] = vx / max_speed;
            input.move_xy[1] = vz / max_speed;
        }
        input.jump = character->input.jump;
        input.crouch = character->input.crouch;
        input.prone = character->input.prone;
        input.lean_left = character->input.lean_left;
        input.lean_right = character->input.lean_right;
        ::psynder::physics::character_tick(character->controller,
                                           input,
                                           world->fixed_dt_seconds);
    }

    ::psynder::physics::tick(world->physics_world, world->fixed_dt_seconds);
}

Transform character_transform(const Character* character) {
    Transform transform{};
    if (!character || !character->controller) return transform;

    ::psynder::physics::character_get_transform(
        character->controller,
        transform.foot_position_m,
        &transform.yaw_deg);
    return transform;
}

} // namespace psynder::physics::character_spine
