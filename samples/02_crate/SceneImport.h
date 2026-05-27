// SPDX-License-Identifier: MIT
// Scene-on-ECS migration, step 2: import a parsed loose-scene document into the
// ECS as canonical prop entities (TransformWS + Collider + RenderMaterial),
// replacing the bespoke std::vector<ScenePrimitive> render/collide path.
//
// Lives at the sample level because it bridges the sample's scene-document
// representation (SceneDocument.h, built on the editor's loose-scene parser) to
// the engine ECS schema (scene/SceneComponents.h). Step 5 rewires 02_crate to
// drive rendering + physics off these entities.

#pragma once

#include "SceneDocument.h"  // psynder::sample02::ParsedSceneDocument / ScenePrimitive

#include "scene/SceneComponents.h"
#include "scene/World.h"

#include "math/Math.h"
#include "math/MathExt.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace psynder::sample02 {

// Compose the primitive's authored Euler angles (degrees) into a quaternion,
// matching the sample's existing Z*Y*X convention so ECS transforms agree with
// the legacy renderer during the migration.
inline psynder::math::Quat euler_deg_to_quat(psynder::math::Vec3 euler_deg) noexcept {
    namespace m = psynder::math;
    const m::Quat rx = m::quat_from_axis_angle({1.0f, 0.0f, 0.0f}, euler_deg.x * m::kDegToRad);
    const m::Quat ry = m::quat_from_axis_angle({0.0f, 1.0f, 0.0f}, euler_deg.y * m::kDegToRad);
    const m::Quat rz = m::quat_from_axis_angle({0.0f, 0.0f, 1.0f}, euler_deg.z * m::kDegToRad);
    return m::quat_normalize(m::quat_mul(rz, m::quat_mul(ry, rx)));
}

inline psynder::scene::ShapeKind to_shape_kind(ScenePrimitiveKind kind) noexcept {
    switch (kind) {
        case ScenePrimitiveKind::Sphere: return psynder::scene::ShapeKind::Sphere;
        case ScenePrimitiveKind::Plane:  return psynder::scene::ShapeKind::Plane;
        case ScenePrimitiveKind::Cube:   break;
    }
    return psynder::scene::ShapeKind::Box;
}

inline psynder::scene::PropDesc prop_from_primitive(const ScenePrimitive& p) {
    psynder::scene::PropDesc d;
    d.position = p.position;
    d.rotation = euler_deg_to_quat(p.rotation_euler_deg);
    d.scale = p.scale;
    d.shape = to_shape_kind(p.kind);

    // Collider half-extents from scale, mirroring the legacy collider mapping.
    const float hx = std::max(0.05f, std::fabs(p.scale.x) * 0.5f);
    const float hy = std::max(0.05f, std::fabs(p.scale.y) * 0.5f);
    const float hz = std::max(0.05f, std::fabs(p.scale.z) * 0.5f);
    if (d.shape == psynder::scene::ShapeKind::Sphere) {
        const float radius =
            std::max(std::fabs(p.scale.x), std::max(std::fabs(p.scale.y), std::fabs(p.scale.z))) *
            0.5f;
        d.half_extents = {std::max(0.05f, radius), 0.0f, 0.0f};
    } else if (d.shape == psynder::scene::ShapeKind::Plane) {
        d.half_extents = {hx, 0.05f, hz};
    } else {
        d.half_extents = {hx, hy, hz};
    }

    d.material.albedo = p.material.albedo;
    d.material.roughness = p.material.roughness;
    d.material.metallic = p.material.metallic;
    d.material.emissive = p.material.emissive;
    d.material.emissive_intensity = p.material.emissive_intensity;
    return d;
}

// Spawn every visible primitive in `doc` as a prop entity. Returns the count.
inline std::size_t import_scene_props(psynder::scene::World& world,
                                      const ParsedSceneDocument& doc) {
    std::size_t count = 0;
    for (const ScenePrimitive& primitive : doc.primitives) {
        if (!primitive.visible) continue;
        psynder::scene::spawn_prop(world, prop_from_primitive(primitive));
        ++count;
    }
    return count;
}

}  // namespace psynder::sample02
