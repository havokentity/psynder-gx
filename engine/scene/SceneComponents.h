// SPDX-License-Identifier: MIT
// Psynder — canonical runtime scene components.
//
// The single schema that the renderer (lane 09 Extract), physics, and gameplay
// all query, so scene::World is the one source of truth for scene content
// (ADR-018 / scene-on-ECS migration). Transform is the renderer's frozen
// TransformWS (GxComponents.h); this header adds the shape/collider and solid
// material that both rendering (which primitive, what surface) and physics
// (collision/raycast bounds) read off the same entity.

#pragma once

#include "scene/GxComponents.h"  // TransformWS + PSYNDER_COMPONENT
#include "scene/World.h"

#include "math/Math.h"

#include "core/Types.h"

namespace psynder::scene {

// Primitive shape shared by render (which builtin mesh) and physics (collider).
// Capsule: a vertical capsule — half_extents.x is the radius, half_extents.y is
// the half-height (total capsule height including the two hemispheres / 2), and
// half_extents.z mirrors the radius. Used by mass agents (humanoid movers).
enum class ShapeKind : u32 { Box = 0, Sphere = 1, Plane = 2, Capsule = 3 };

PSYNDER_COMPONENT(Collider) {
    ShapeKind           kind;
    psynder::math::Vec3 half_extents;  // Sphere: radius in .x; Capsule: radius .x, half-height .y
};

PSYNDER_COMPONENT(RenderMaterial) {
    psynder::math::Vec3 albedo;
    f32                 roughness;
    f32                 metallic;
    psynder::math::Vec3 emissive;
    f32                 emissive_intensity;
};

// Marks a Collider entity as a dynamic Jolt rigid body (ADR-019 class 2):
// gravity-driven, stackable, knocked by impulses, solved by Jolt and synced
// back to TransformWS each tick. Absence of this component => the collider is
// static (projected into Jolt once, immovable for the session).
PSYNDER_COMPONENT(DynamicBody) {
    f32 mass_kg;      // > 0; 0 would be static
    f32 friction;
    f32 restitution;
};

// Scale that maps a unit builtin mesh (half-extent 0.5; see BuiltinMeshes.cpp)
// onto a collider's half_extents, so the drawn primitive matches the collider.
inline psynder::math::Vec3 render_scale_for_collider(const Collider& c) noexcept {
    if (c.kind == ShapeKind::Sphere) {
        const f32 d = c.half_extents.x * 2.0f;
        return {d, d, d};
    }
    return {c.half_extents.x * 2.0f, c.half_extents.y * 2.0f, c.half_extents.z * 2.0f};
}

// Compose a world matrix from translation / rotation / scale (column-major,
// T * R * S — translation lives in the last column).
inline psynder::math::Mat4 mat4_trs(psynder::math::Vec3 position,
                                    psynder::math::Quat rotation,
                                    psynder::math::Vec3 scale) {
    using psynder::math::mul;
    return mul(psynder::math::translate(position),
               mul(psynder::math::rotate_quat(rotation), psynder::math::scale(scale)));
}

// A static scene prop: how a level primitive is spawned into the ECS.
struct PropDesc {
    psynder::math::Vec3 position{0.0f, 0.0f, 0.0f};
    psynder::math::Quat rotation{0.0f, 0.0f, 0.0f, 1.0f};
    psynder::math::Vec3 scale{1.0f, 1.0f, 1.0f};
    ShapeKind           shape = ShapeKind::Box;
    psynder::math::Vec3 half_extents{0.5f, 0.5f, 0.5f};
    RenderMaterial      material{{0.76f, 0.82f, 0.88f}, 0.65f, 0.0f, {0.0f, 0.0f, 0.0f}, 0.0f};
};

// Spawn a prop entity carrying the canonical render + physics components.
inline Entity spawn_prop(World& world, const PropDesc& desc) {
    const Entity e = world.create();
    TransformWS transform{};
    transform.mtw = mat4_trs(desc.position, desc.rotation, desc.scale);
    transform.prev_mtw = transform.mtw;
    world.add(e, transform);
    world.add(e, Collider{desc.shape, desc.half_extents});
    world.add(e, desc.material);
    return e;
}

// A dynamic scene prop (a Jolt rigid body): falls, stacks, reacts to impulses.
// Render scale is derived from half_extents (builtin meshes are unit-sized), so
// the drawn primitive matches the collider exactly.
struct DynamicPropDesc {
    psynder::math::Vec3 position{0.0f, 0.0f, 0.0f};
    psynder::math::Quat rotation{0.0f, 0.0f, 0.0f, 1.0f};
    ShapeKind           shape = ShapeKind::Box;
    psynder::math::Vec3 half_extents{0.5f, 0.5f, 0.5f};
    f32                 mass_kg = 8.0f;
    f32                 friction = 0.5f;
    f32                 restitution = 0.1f;
    RenderMaterial      material{{0.85f, 0.45f, 0.20f}, 0.7f, 0.0f, {0.0f, 0.0f, 0.0f}, 0.0f};
};

// Spawn a dynamic rigid-body prop. Carries the same render + collider schema as
// a static prop plus a DynamicBody marker, so the renderer/raycast treat it
// identically while the physics bridge drives it through Jolt.
inline Entity spawn_dynamic_prop(World& world, const DynamicPropDesc& desc) {
    const Entity e = world.create();
    const Collider collider{desc.shape, desc.half_extents};
    TransformWS transform{};
    transform.mtw = mat4_trs(desc.position, desc.rotation, render_scale_for_collider(collider));
    transform.prev_mtw = transform.mtw;
    world.add(e, transform);
    world.add(e, collider);
    world.add(e, desc.material);
    world.add(e, DynamicBody{desc.mass_kg, desc.friction, desc.restitution});
    return e;
}

}  // namespace psynder::scene
