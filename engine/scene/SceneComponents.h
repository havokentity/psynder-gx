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
enum class ShapeKind : u32 { Box = 0, Sphere = 1, Plane = 2 };

PSYNDER_COMPONENT(Collider) {
    ShapeKind           kind;
    psynder::math::Vec3 half_extents;  // Sphere: radius in half_extents.x
};

PSYNDER_COMPONENT(RenderMaterial) {
    psynder::math::Vec3 albedo;
    f32                 roughness;
    f32                 metallic;
    psynder::math::Vec3 emissive;
    f32                 emissive_intensity;
};

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

}  // namespace psynder::scene
