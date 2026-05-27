// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/physics/core/CharacterController.h
//
// Scene-on-ECS migration, step 4: the player capsule + its collision against
// the static scene as ECS systems over scene::World — NOT a separate
// character_spine::World owning a parallel truth.
//
// The character is an entity carrying:
//   * TransformWS      — translation column == capsule FOOT position (metres),
//                        matching the foot-origin convention of CharacterSpine.
//   * CharacterController — capsule params + world velocity + grounded flag.
//   * CharacterInput   — desired move direction / speed / jump for this tick.
//
// Two systems run each fixed tick (DESIGN §3 — functions over World, DOTS
// chunk iteration, no per-entity virtual dispatch):
//   1. character_input_system(World&)            input  → desired velocity
//   2. character_physics_step(World&, dt, g)     integrate + collide vs static
//                                                 Colliders → write TransformWS
//
// Determinism (DESIGN §14 + cmake/HotLane.cmake psynder_determinism_fp): the
// resolver is pure deterministic C++ (fixed tick, no fast-math reordering, no
// Jolt). The capsule is approximated by its world AABB and resolved against
// each static Collider's AABB along the axis of least penetration — enough to
// walk on the ground, bump crates, and jump, while staying bit-reproducible
// (two identical worlds tick to identical transforms).

#pragma once

#include "scene/SceneComponents.h"
#include "scene/World.h"

#include "math/Math.h"

#include "core/Types.h"

#include <cmath>

namespace psynder::physics {

// Player capsule state. Foot position lives in TransformWS (translation).
PSYNDER_COMPONENT(CharacterController) {
    f32 radius_m;            // capsule radius (metres)
    f32 height_m;            // total capsule height incl. hemispheres (metres)
    psynder::math::Vec3 velocity;  // world-space velocity (m/s)
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

// Create a character entity with the canonical capsule components.
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

// System 1 — input → desired velocity. Reads CharacterInput, writes the
// desired horizontal velocity + jump edge into CharacterController.
inline void character_input_system(scene::World& world) {
    world.for_each_chunk<CharacterInput, CharacterController>(
        [](std::size_t n, CharacterInput* in, CharacterController* cc) {
            for (std::size_t i = 0; i < n; ++i) {
                cc[i].desired_vx = in[i].move_dir_x * in[i].speed_mps;
                cc[i].desired_vz = in[i].move_dir_z * in[i].speed_mps;
                cc[i].jump_requested = in[i].jump;
            }
        });
}

namespace detail {

// World-space AABB of a static Collider entity (its TransformWS translation
// is the centre; half-extents come straight off the Collider). Boxes and
// planes use half_extents directly; spheres use the radius (in x) as a cube.
struct Aabb {
    psynder::math::Vec3 center;
    psynder::math::Vec3 half;
};

inline Aabb collider_aabb(const scene::TransformWS& xf, const scene::Collider& c) noexcept {
    Aabb a;
    a.center = {xf.mtw.m[12], xf.mtw.m[13], xf.mtw.m[14]};
    if (c.kind == scene::ShapeKind::Sphere) {
        const f32 r = c.half_extents.x;
        a.half = {r, r, r};
    } else {
        a.half = c.half_extents;
    }
    return a;
}

// Capsule's world AABB given its foot position.
inline Aabb capsule_aabb(psynder::math::Vec3 foot, f32 radius, f32 height) noexcept {
    Aabb a;
    a.center = {foot.x, foot.y + height * 0.5f, foot.z};
    a.half   = {radius, height * 0.5f, radius};
    return a;
}

}  // namespace detail

// System 2 — integrate velocity under gravity, resolve against the static
// scene Colliders, and write the new foot position back to TransformWS.
// dt is the fixed tick in seconds; gravity is world acceleration (m/s^2,
// gravity.y is negative). Read-only nested walk over static Colliders is
// allowed inside the outer character walk (no structural change).
inline void character_physics_step(scene::World& world, f32 dt,
                                   psynder::math::Vec3 gravity) {
    world.for_each_chunk<scene::TransformWS, CharacterController>(
        [&](std::size_t n, scene::TransformWS* xf, CharacterController* cc) {
            for (std::size_t i = 0; i < n; ++i) {
                CharacterController& c = cc[i];
                psynder::math::Vec3 foot{xf[i].mtw.m[12], xf[i].mtw.m[13],
                                         xf[i].mtw.m[14]};

                // Horizontal velocity is input-driven; vertical integrates g.
                c.velocity.x = c.desired_vx;
                c.velocity.z = c.desired_vz;
                c.velocity.y += gravity.y * dt;
                if (c.jump_requested && c.grounded) {
                    c.velocity.y = c.jump_speed;
                }
                c.jump_requested = 0;
                c.grounded = 0;

                foot.x += c.velocity.x * dt;
                foot.y += c.velocity.y * dt;
                foot.z += c.velocity.z * dt;

                // Resolve against every static Collider, a few iterations for
                // stability. Min-penetration-axis push-out (AABB vs AABB).
                for (int iter = 0; iter < 4; ++iter) {
                    world.for_each_chunk<scene::TransformWS, scene::Collider>(
                        [&](std::size_t m, scene::TransformWS* sxf,
                            scene::Collider* col) {
                            for (std::size_t j = 0; j < m; ++j) {
                                const detail::Aabb cap =
                                    detail::capsule_aabb(foot, c.radius_m, c.height_m);
                                const detail::Aabb box =
                                    detail::collider_aabb(sxf[j], col[j]);

                                const f32 dx = cap.center.x - box.center.x;
                                const f32 px = (cap.half.x + box.half.x) - std::fabs(dx);
                                if (px <= 0.0f) return;
                                const f32 dy = cap.center.y - box.center.y;
                                const f32 py = (cap.half.y + box.half.y) - std::fabs(dy);
                                if (py <= 0.0f) return;
                                const f32 dz = cap.center.z - box.center.z;
                                const f32 pz = (cap.half.z + box.half.z) - std::fabs(dz);
                                if (pz <= 0.0f) return;

                                // Penetrating on all axes — push out along the
                                // axis of least penetration.
                                if (py <= px && py <= pz) {
                                    if (dy >= 0.0f) {  // capsule above: stand on top
                                        foot.y += py;
                                        if (c.velocity.y < 0.0f) c.velocity.y = 0.0f;
                                        c.grounded = 1;
                                    } else {           // capsule below: hit ceiling
                                        foot.y -= py;
                                        if (c.velocity.y > 0.0f) c.velocity.y = 0.0f;
                                    }
                                } else if (px <= pz) {
                                    foot.x += (dx >= 0.0f) ? px : -px;
                                    c.velocity.x = 0.0f;
                                } else {
                                    foot.z += (dz >= 0.0f) ? pz : -pz;
                                    c.velocity.z = 0.0f;
                                }
                            }
                        });
                }

                xf[i].prev_mtw = xf[i].mtw;
                xf[i].mtw = foot_transform(foot);
            }
        });
}

}  // namespace psynder::physics
