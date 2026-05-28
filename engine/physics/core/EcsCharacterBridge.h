// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/physics/core/EcsCharacterBridge.h
//
// Bridges the canonical scene ECS (scene::World, scene::Collider/TransformWS)
// to the Jolt-backed character_spine. The ECS stays authoritative: static
// colliders are PROJECTED into a Jolt world at play-start (they don't move),
// the player runs as a Jolt CharacterVirtual, and the solved transform is
// written back into the player's ECS TransformWS each tick. No parallel
// MUTABLE scene — Jolt is the solver, the ECS is the truth.
//
// This is the "use Jolt for the hard dynamics" half of the hybrid physics
// architecture (see docs/adr/ADR-019). The homogeneous many-agent path is a
// separate DOTS system that also writes TransformWS.

#pragma once

#include "physics/core/CharacterSpine.h"

#include "scene/SceneComponents.h"
#include "scene/World.h"

#include "math/Math.h"
#include "core/Types.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>
#include <vector>

namespace psynder::physics {

// One ECS static collider projected into the Jolt world: the source entity and
// the Jolt body handle it produced. Returned by build_jolt_statics_from_ecs so
// the caller can later remove_static_body() the body of a destroyed prop.
using EcsStaticBody = std::pair<psynder::Entity, character_spine::StaticBodyHandle>;

// Recover the pure rotation quaternion from a TransformWS (strips scale out of
// the mtw basis columns, then matrix->quat via the standard branch method).
inline psynder::math::Quat rotation_from_transform(const scene::TransformWS& xf) noexcept {
    const f32* m = xf.mtw.m;
    auto col_len = [](f32 x, f32 y, f32 z) {
        const f32 l = std::sqrt(x * x + y * y + z * z);
        return l > 1e-8f ? l : 1.0f;
    };
    const f32 sx = col_len(m[0], m[1], m[2]);
    const f32 sy = col_len(m[4], m[5], m[6]);
    const f32 sz = col_len(m[8], m[9], m[10]);
    // Column-major mtw -> rotation matrix R[row][col].
    const f32 r00 = m[0] / sx, r10 = m[1] / sx, r20 = m[2] / sx;
    const f32 r01 = m[4] / sy, r11 = m[5] / sy, r21 = m[6] / sy;
    const f32 r02 = m[8] / sz, r12 = m[9] / sz, r22 = m[10] / sz;

    psynder::math::Quat q{};
    const f32 trace = r00 + r11 + r22;
    if (trace > 0.0f) {
        f32 s = std::sqrt(trace + 1.0f) * 2.0f;  // s = 4w
        q.w = 0.25f * s;
        q.x = (r21 - r12) / s;
        q.y = (r02 - r20) / s;
        q.z = (r10 - r01) / s;
    } else if (r00 > r11 && r00 > r22) {
        f32 s = std::sqrt(1.0f + r00 - r11 - r22) * 2.0f;  // s = 4x
        q.w = (r21 - r12) / s;
        q.x = 0.25f * s;
        q.y = (r01 + r10) / s;
        q.z = (r02 + r20) / s;
    } else if (r11 > r22) {
        f32 s = std::sqrt(1.0f + r11 - r00 - r22) * 2.0f;  // s = 4y
        q.w = (r02 - r20) / s;
        q.x = (r01 + r10) / s;
        q.y = 0.25f * s;
        q.z = (r12 + r21) / s;
    } else {
        f32 s = std::sqrt(1.0f + r22 - r00 - r11) * 2.0f;  // s = 4z
        q.w = (r10 - r01) / s;
        q.x = (r02 + r20) / s;
        q.y = (r12 + r21) / s;
        q.z = 0.25f * s;
    }
    return q;
}

// Add a Jolt static body for every ECS Collider entity in `ecs`. Box/Plane map
// to static boxes (a Plane's thin box is dropped so its top face sits at the
// authored surface); Sphere maps to a static sphere. Returns the entity->handle
// mapping so a caller can remove the Jolt body when its ECS entity is destroyed
// (e.g. a shot crate) and avoid leaving an invisible collider behind.
inline std::vector<EcsStaticBody> build_jolt_statics_from_ecs(
    character_spine::World* world, scene::World& ecs) {
    std::vector<EcsStaticBody> mapping;
    if (!world) return mapping;
    ecs.for_each_chunk_with_entities<scene::TransformWS, scene::Collider>(
        [&](std::size_t n, const psynder::Entity* ents,
            scene::TransformWS* xf, scene::Collider* col) {
            for (std::size_t i = 0; i < n; ++i) {
                // Dynamic bodies are projected separately (and move) — never as
                // immovable statics, or they'd get two colliders.
                if (ecs.get<scene::DynamicBody>(ents[i]) != nullptr) continue;
                const psynder::math::Quat q = rotation_from_transform(xf[i]);
                const f32* m = xf[i].mtw.m;
                character_spine::StaticBodyHandle handle{};
                if (col[i].kind == scene::ShapeKind::Sphere) {
                    character_spine::SphereDesc d{};
                    d.center_m[0] = m[12];
                    d.center_m[1] = m[13];
                    d.center_m[2] = m[14];
                    d.rotation_quat[0] = q.x;
                    d.rotation_quat[1] = q.y;
                    d.rotation_quat[2] = q.z;
                    d.rotation_quat[3] = q.w;
                    d.radius_m = std::max(0.05f, col[i].half_extents.x);
                    handle = character_spine::add_static_sphere(world, d);
                } else {
                    character_spine::BoxDesc d{};
                    f32 cy = m[13];
                    if (col[i].kind == scene::ShapeKind::Plane) {
                        cy -= col[i].half_extents.y;  // top face at the surface
                    }
                    d.center_m[0] = m[12];
                    d.center_m[1] = cy;
                    d.center_m[2] = m[14];
                    d.rotation_quat[0] = q.x;
                    d.rotation_quat[1] = q.y;
                    d.rotation_quat[2] = q.z;
                    d.rotation_quat[3] = q.w;
                    d.half_extents_m[0] = std::max(0.05f, col[i].half_extents.x);
                    d.half_extents_m[1] = std::max(0.05f, col[i].half_extents.y);
                    d.half_extents_m[2] = std::max(0.05f, col[i].half_extents.z);
                    handle = character_spine::add_static_box(world, d);
                }
                if (handle.valid()) mapping.push_back({ents[i], handle});
            }
        });
    return mapping;
}

// One ECS dynamic collider projected into the Jolt world: source entity + the
// dynamic body handle. The handle is read back each tick (sync_dynamics_to_ecs).
using EcsDynamicBody = std::pair<psynder::Entity, character_spine::DynamicBodyHandle>;

// Spawn a Jolt dynamic rigid body for every ECS entity carrying a DynamicBody
// (mass > 0) alongside TransformWS + Collider (ADR-019 class 2). Box/Plane map
// to dynamic boxes, Sphere to a dynamic sphere. Returns the entity->handle map.
inline std::vector<EcsDynamicBody> build_jolt_dynamics_from_ecs(
    character_spine::World* world, scene::World& ecs) {
    std::vector<EcsDynamicBody> mapping;
    if (!world) return mapping;
    ecs.for_each_chunk_with_entities<scene::TransformWS, scene::Collider, scene::DynamicBody>(
        [&](std::size_t n, const psynder::Entity* ents, scene::TransformWS* xf,
            scene::Collider* col, scene::DynamicBody* dyn) {
            for (std::size_t i = 0; i < n; ++i) {
                if (dyn[i].mass_kg <= 0.0f) continue;
                const psynder::math::Quat q = rotation_from_transform(xf[i]);
                const f32* m = xf[i].mtw.m;
                character_spine::DynamicBodyHandle handle{};
                if (col[i].kind == scene::ShapeKind::Sphere) {
                    character_spine::DynamicSphereDesc d{};
                    d.center_m[0] = m[12];
                    d.center_m[1] = m[13];
                    d.center_m[2] = m[14];
                    d.rotation_quat[0] = q.x;
                    d.rotation_quat[1] = q.y;
                    d.rotation_quat[2] = q.z;
                    d.rotation_quat[3] = q.w;
                    d.radius_m = std::max(0.05f, col[i].half_extents.x);
                    d.mass_kg = dyn[i].mass_kg;
                    d.friction = dyn[i].friction;
                    d.restitution = dyn[i].restitution;
                    handle = character_spine::add_dynamic_sphere(world, d);
                } else {
                    character_spine::DynamicBoxDesc d{};
                    d.center_m[0] = m[12];
                    d.center_m[1] = m[13];
                    d.center_m[2] = m[14];
                    d.rotation_quat[0] = q.x;
                    d.rotation_quat[1] = q.y;
                    d.rotation_quat[2] = q.z;
                    d.rotation_quat[3] = q.w;
                    d.half_extents_m[0] = std::max(0.05f, col[i].half_extents.x);
                    d.half_extents_m[1] = std::max(0.05f, col[i].half_extents.y);
                    d.half_extents_m[2] = std::max(0.05f, col[i].half_extents.z);
                    d.mass_kg = dyn[i].mass_kg;
                    d.friction = dyn[i].friction;
                    d.restitution = dyn[i].restitution;
                    handle = character_spine::add_dynamic_box(world, d);
                }
                if (handle.valid()) mapping.push_back({ents[i], handle});
            }
        });
    return mapping;
}

// Write each dynamic body's Jolt-solved transform back into its ECS TransformWS
// (the ECS stays authoritative; render/gameplay/raycast read only the ECS).
// Call once per frame after stepping. Render scale is re-derived from the
// collider so the drawn primitive keeps matching the body.
inline void sync_dynamics_to_ecs(const character_spine::World* world,
                                 scene::World& ecs,
                                 const std::vector<EcsDynamicBody>& mapping) {
    if (!world) return;
    for (const auto& [entity, handle] : mapping) {
        float pos[3];
        float quat[4];
        if (!character_spine::dynamic_body_transform(world, handle, pos, quat)) continue;
        scene::TransformWS* xf = ecs.get<scene::TransformWS>(entity);
        if (!xf) continue;
        const scene::Collider* col = ecs.get<scene::Collider>(entity);
        const psynder::math::Vec3 scale =
            col ? scene::render_scale_for_collider(*col)
                : psynder::math::Vec3{1.0f, 1.0f, 1.0f};
        psynder::math::Quat q{};
        q.x = quat[0];
        q.y = quat[1];
        q.z = quat[2];
        q.w = quat[3];
        xf->prev_mtw = xf->mtw;
        xf->mtw = scene::mat4_trs({pos[0], pos[1], pos[2]}, q, scale);
    }
}

}  // namespace psynder::physics
