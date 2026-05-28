// SPDX-License-Identifier: MIT
// Psynder — reference FPS combat slice, built entirely on the ECS spine.
//
// This is the worked example of the ADR-018 execution model end to end:
//
//     query (for_each_chunk)  ->  raycast  ->  command buffer (deferred effects)
//
// A weapon fires a hitscan ray; the nearest entity with a collider takes
// damage; a reaper system destroys anything whose health hits zero and drops a
// pickup where it died — and every structural change is deferred to a
// CommandBuffer replayed at a sync point, never applied mid-iteration.
//
// Header-only and renderer-agnostic: this is gameplay logic over psynder::scene,
// reusable from any sample or the editor's play mode. Nothing here touches the
// GPU; wiring it to the visible scene is the separate scene-on-ECS milestone.

#pragma once

#include "scene/CommandBuffer.h"
#include "scene/SceneComponents.h"  // canonical Collider / ShapeKind + TransformWS
#include "scene/World.h"

#include "math/Bounds.h"
#include "math/Math.h"

#include "core/Types.h"

#include <algorithm>
#include <cmath>

namespace psynder::combat {

using psynder::math::Vec3;

// ─── Components (POD, trivially copyable — DOTS contract) ──────────────────
// Position + collider are the CANONICAL scene components (scene::TransformWS +
// scene::Collider), the same ones the renderer and the character controller use
// — so a hitscan strikes exactly the props you see and walk into. Health/Pickup
// are combat-specific.
PSYNDER_COMPONENT(Health)      { f32 hp; f32 max_hp; };
PSYNDER_COMPONENT(Pickup)      { u32 kind; };           ///< dropped on death

// World position from a TransformWS (its translation column).
inline Vec3 world_position(const psynder::scene::TransformWS& xf) noexcept {
    return {xf.mtw.m[12], xf.mtw.m[13], xf.mtw.m[14]};
}

// AABB half-extents of a collider (sphere uses its radius on every axis).
inline Vec3 collider_half_extents(const psynder::scene::Collider& c) noexcept {
    if (c.kind == psynder::scene::ShapeKind::Sphere) {
        return {c.half_extents.x, c.half_extents.x, c.half_extents.x};
    }
    return c.half_extents;
}

// ─── Hitscan ───────────────────────────────────────────────────────────────
struct Ray {
    Vec3 origin{0.0f, 0.0f, 0.0f};
    Vec3 dir{0.0f, 0.0f, 1.0f};  // need not be unit length
};

struct Hit {
    psynder::Entity entity{};
    f32                    t = 0.0f;  // distance along dir to the entry point
    bool                   hit = false;
};

// Slab test: nearest entry t of `ray` into `box`, clamped to [0, max_t].
inline bool ray_aabb(const Ray& ray, const psynder::math::Aabb& box, f32 max_t, f32& out_t) {
    const f32 o[3]  = {ray.origin.x, ray.origin.y, ray.origin.z};
    const f32 d[3]  = {ray.dir.x, ray.dir.y, ray.dir.z};
    const f32 lo[3] = {box.min.x, box.min.y, box.min.z};
    const f32 hi[3] = {box.max.x, box.max.y, box.max.z};

    f32 tmin = 0.0f;
    f32 tmax = max_t;
    for (int axis = 0; axis < 3; ++axis) {
        if (std::fabs(d[axis]) < 1e-8f) {
            if (o[axis] < lo[axis] || o[axis] > hi[axis]) return false;  // parallel, outside
            continue;
        }
        const f32 inv = 1.0f / d[axis];
        f32 t1 = (lo[axis] - o[axis]) * inv;
        f32 t2 = (hi[axis] - o[axis]) * inv;
        if (t1 > t2) std::swap(t1, t2);
        tmin = std::max(tmin, t1);
        tmax = std::min(tmax, t2);
        if (tmin > tmax) return false;
    }
    out_t = tmin;
    return true;
}

// Fire a hitscan ray and return the nearest entity (with TransformWS + Collider)
// it strikes within max_range. Brute-force over the matching chunks — fully
// cache-coherent; swap in the spatial index (engine/scene/Spatial.h) once entity
// counts make a broadphase worthwhile. Colliders are tested as world AABBs
// (rotation-aware OBB tests are a later refinement).
inline Hit raycast_nearest(psynder::scene::World& world, const Ray& ray, f32 max_range) {
    Hit best{};
    best.t = max_range;
    world.for_each_chunk_with_entities<psynder::scene::TransformWS, psynder::scene::Collider>(
        [&](usize n, const psynder::Entity* ents,
            psynder::scene::TransformWS* xf, psynder::scene::Collider* col) {
            for (usize i = 0; i < n; ++i) {
                const psynder::math::Aabb box = psynder::math::aabb_from_center_extents(
                    world_position(xf[i]), collider_half_extents(col[i]));
                f32 t = 0.0f;
                if (ray_aabb(ray, box, max_range, t) && t < best.t) {
                    best.hit = true;
                    best.t = t;
                    best.entity = ents[i];
                }
            }
        });
    return best;
}

// Apply damage to a hit entity's Health, if it has one.
inline void apply_damage(psynder::scene::World& world, const Hit& hit, f32 damage) {
    if (!hit.hit) return;
    if (Health* h = world.get<Health>(hit.entity)) {
        h->hp -= damage;
    }
}

// Reaper system: destroy every entity at or below zero health and drop a Pickup
// where it died. Structural changes are recorded into `cb` and applied at the
// next playback() — never inline during iteration.
inline void reap_dead(psynder::scene::World& world,
                      psynder::scene::CommandBuffer& cb,
                      u32 pickup_kind = 1u) {
    world.for_each_chunk_with_entities<Health, psynder::scene::TransformWS>(
        [&](usize n, const psynder::Entity* ents, Health* h, psynder::scene::TransformWS* xf) {
            for (usize i = 0; i < n; ++i) {
                if (h[i].hp <= 0.0f) {
                    cb.destroy(ents[i]);
                    const psynder::Entity drop = cb.create();
                    psynder::scene::TransformWS dxf{};
                    dxf.mtw = psynder::math::translate(world_position(xf[i]));
                    dxf.prev_mtw = dxf.mtw;
                    cb.add(drop, dxf);
                    cb.add(drop, Pickup{pickup_kind});
                }
            }
        });
}

}  // namespace psynder::combat
