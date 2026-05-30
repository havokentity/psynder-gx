// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/physics/core/DynamicBody.cpp
//
// Implementation of the dynamic Jolt rigid-body spine (see DynamicBody.h).
// Mirrors CharacterSpine.cpp's wrapper structure: an opaque `World` owning a
// psynder::physics::World plus body bookkeeping, monotonic never-reused ids,
// swap-remove on delete, and a fixed-dt step. No exceptions / RTTI / shared_ptr
// in this hot lane; allocation failure surfaces as nullptr / invalid handles.

#include "physics/core/DynamicBody.h"

#include "physics/core/PublicPhysicsCore.h"

#include <cstddef>
#include <new>
#include <vector>

namespace psynder::physics::dynamic_body {

namespace {

// One static (mass 0) body: its stable handle id and the backing rigid body.
struct StaticBody {
    std::uint64_t id = 0;
    ::psynder::physics::RigidBody* body = nullptr;
};

// One dynamic (mass > 0) body: its stable handle id and the backing rigid body.
struct DynamicBody {
    std::uint64_t id = 0;
    ::psynder::physics::RigidBody* body = nullptr;
};

}  // namespace

struct World {
    ::psynder::physics::World* physics_world = nullptr;
    std::uint32_t tick_hz = 120;
    float fixed_dt_seconds = 1.0f / 120.0f;
    std::vector<StaticBody> static_bodies;
    std::vector<DynamicBody> dynamic_bodies;
    // 0 is the reserved invalid handle id; ids are monotonic across BOTH kinds
    // so a static and a dynamic handle can never collide, and an id is never
    // reused for the life of the world (stale handles stay safely invalid).
    std::uint64_t next_body_id = 1;
};

namespace {

// Finds a dynamic body by handle id; returns nullptr if absent. const + mutable
// overloads so readers (dynamic_body_position) and mutators share the lookup.
const DynamicBody* find_dynamic(const World* world, DynamicBodyHandle h) noexcept {
    if (!world || !h.valid()) return nullptr;
    for (const DynamicBody& entry : world->dynamic_bodies) {
        if (entry.id == h.id) return &entry;
    }
    return nullptr;
}

DynamicBody* find_dynamic(World* world, DynamicBodyHandle h) noexcept {
    if (!world || !h.valid()) return nullptr;
    for (DynamicBody& entry : world->dynamic_bodies) {
        if (entry.id == h.id) return &entry;
    }
    return nullptr;
}

}  // namespace

World* create_world(const WorldDesc& desc) noexcept {
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

void destroy_world(World* world) noexcept {
    if (!world) return;

    for (const StaticBody& entry : world->static_bodies) {
        ::psynder::physics::destroy_body(world->physics_world, entry.body);
    }
    world->static_bodies.clear();

    for (const DynamicBody& entry : world->dynamic_bodies) {
        ::psynder::physics::destroy_body(world->physics_world, entry.body);
    }
    world->dynamic_bodies.clear();

    ::psynder::physics::destroy_world(world->physics_world);
    delete world;
}

void step_fixed(World* world) noexcept {
    if (!world || !world->physics_world) return;
    // Advance the deterministic Jolt simulation by exactly one fixed dt. Any
    // per-step force (from apply_impulse) was added before this and is cleared
    // by the backend after the Update, so impulses are one-tick events.
    ::psynder::physics::tick(world->physics_world, world->fixed_dt_seconds);
}

StaticBodyHandle add_static_box(World* world, math::Vec3 half_extents,
                                math::Vec3 pos) noexcept {
    if (!world || !world->physics_world) return {};
    if (half_extents.x <= 0.0f || half_extents.y <= 0.0f ||
        half_extents.z <= 0.0f) {
        return {};
    }

    ::psynder::physics::BodyDesc body{};
    body.shape = ::psynder::physics::Shape::Box;
    body.pos[0] = pos.x;
    body.pos[1] = pos.y;
    body.pos[2] = pos.z;
    body.rot_quat[0] = 0.0f;
    body.rot_quat[1] = 0.0f;
    body.rot_quat[2] = 0.0f;
    body.rot_quat[3] = 1.0f;
    body.mass_kg = 0.0f;  // 0 = static / immovable.
    body.friction = 0.8f;
    body.restitution = 0.0f;
    body.dims[0] = half_extents.x;
    body.dims[1] = half_extents.y;
    body.dims[2] = half_extents.z;

    ::psynder::physics::RigidBody* rigid_body =
        ::psynder::physics::create_body(world->physics_world, body);
    if (!rigid_body) return {};

    const std::uint64_t id = world->next_body_id++;
    world->static_bodies.push_back(StaticBody{id, rigid_body});
    return StaticBodyHandle{id};
}

StaticBodyHandle add_static_ground(World* world, float half_extent_x,
                                   float half_extent_z, float top_y) noexcept {
    // A 1 m thick slab whose top face is at top_y: centre half a metre below it.
    return add_static_box(world, {half_extent_x, 0.5f, half_extent_z},
                          {0.0f, top_y - 0.5f, 0.0f});
}

DynamicBodyHandle create_dynamic_box(World* world, math::Vec3 half_extents,
                                     math::Vec3 pos, float mass_kg) noexcept {
    if (!world || !world->physics_world) return {};
    if (half_extents.x <= 0.0f || half_extents.y <= 0.0f ||
        half_extents.z <= 0.0f || mass_kg <= 0.0f) {
        return {};
    }

    ::psynder::physics::BodyDesc body{};
    body.shape = ::psynder::physics::Shape::Box;
    body.pos[0] = pos.x;
    body.pos[1] = pos.y;
    body.pos[2] = pos.z;
    body.rot_quat[0] = 0.0f;
    body.rot_quat[1] = 0.0f;
    body.rot_quat[2] = 0.0f;
    body.rot_quat[3] = 1.0f;
    body.mass_kg = mass_kg;
    body.friction = 0.5f;
    body.restitution = 0.1f;
    body.dims[0] = half_extents.x;
    body.dims[1] = half_extents.y;
    body.dims[2] = half_extents.z;

    ::psynder::physics::RigidBody* rigid_body =
        ::psynder::physics::create_body(world->physics_world, body);
    if (!rigid_body) return {};

    const std::uint64_t id = world->next_body_id++;
    world->dynamic_bodies.push_back(DynamicBody{id, rigid_body});
    return DynamicBodyHandle{id};
}

DynamicBodyHandle create_dynamic_sphere(World* world, float radius,
                                        math::Vec3 pos, float mass_kg) noexcept {
    if (!world || !world->physics_world) return {};
    if (radius <= 0.0f || mass_kg <= 0.0f) return {};

    ::psynder::physics::BodyDesc body{};
    body.shape = ::psynder::physics::Shape::Sphere;
    body.pos[0] = pos.x;
    body.pos[1] = pos.y;
    body.pos[2] = pos.z;
    body.rot_quat[0] = 0.0f;
    body.rot_quat[1] = 0.0f;
    body.rot_quat[2] = 0.0f;
    body.rot_quat[3] = 1.0f;
    body.mass_kg = mass_kg;
    body.friction = 0.5f;
    body.restitution = 0.1f;
    body.dims[0] = radius;

    ::psynder::physics::RigidBody* rigid_body =
        ::psynder::physics::create_body(world->physics_world, body);
    if (!rigid_body) return {};

    const std::uint64_t id = world->next_body_id++;
    world->dynamic_bodies.push_back(DynamicBody{id, rigid_body});
    return DynamicBodyHandle{id};
}

bool remove_dynamic_body(World* world, DynamicBodyHandle handle) noexcept {
    if (!world || !world->physics_world || !handle.valid()) return false;
    auto& bodies = world->dynamic_bodies;
    for (std::size_t i = 0; i < bodies.size(); ++i) {
        if (bodies[i].id != handle.id) continue;
        ::psynder::physics::destroy_body(world->physics_world, bodies[i].body);
        // Dynamic bodies are independent, so order doesn't matter: swap-remove.
        bodies[i] = bodies.back();
        bodies.pop_back();
        return true;
    }
    return false;
}

math::Vec3 dynamic_body_position(const World* world,
                                 DynamicBodyHandle handle) noexcept {
    const DynamicBody* entry = find_dynamic(world, handle);
    if (!entry) return {0.0f, 0.0f, 0.0f};
    float pos[3] = {0.0f, 0.0f, 0.0f};
    float quat[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    ::psynder::physics::body_get_transform(entry->body, pos, quat);
    return {pos[0], pos[1], pos[2]};
}

bool dynamic_body_exists(const World* world,
                         DynamicBodyHandle handle) noexcept {
    return find_dynamic(world, handle) != nullptr;
}

void apply_impulse(World* world, DynamicBodyHandle handle,
                   math::Vec3 impulse) noexcept {
    DynamicBody* entry = find_dynamic(world, handle);
    if (!entry) return;
    // The backend takes a force (N) accumulated and cleared each step. Convert
    // the requested impulse (kg·m/s) into a single-tick force: F = J / dt, so
    // exactly one step_fixed delivers the full momentum change. Mirrors the
    // CharacterSpine impulse path.
    const float inv_dt =
        world->fixed_dt_seconds > 0.0f ? 1.0f / world->fixed_dt_seconds : 0.0f;
    ::psynder::physics::body_apply_force(entry->body, impulse.x * inv_dt,
                                         impulse.y * inv_dt, impulse.z * inv_dt);
}

}  // namespace psynder::physics::dynamic_body
