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

namespace psynder::physics {

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
// authored surface); Sphere maps to a static sphere. Returns the body count.
inline std::size_t build_jolt_statics_from_ecs(character_spine::World* world,
                                               scene::World& ecs) {
    if (!world) return 0;
    std::size_t count = 0;
    ecs.for_each_chunk<scene::TransformWS, scene::Collider>(
        [&](std::size_t n, scene::TransformWS* xf, scene::Collider* col) {
            for (std::size_t i = 0; i < n; ++i) {
                const psynder::math::Quat q = rotation_from_transform(xf[i]);
                const f32* m = xf[i].mtw.m;
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
                    if (character_spine::add_static_sphere(world, d)) ++count;
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
                    if (character_spine::add_static_box(world, d)) ++count;
                }
            }
        });
    return count;
}

}  // namespace psynder::physics
