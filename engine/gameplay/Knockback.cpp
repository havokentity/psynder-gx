// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/gameplay/Knockback.cpp — see Knockback.h.

#include "gameplay/Knockback.h"

#include "scene/GxComponents.h"  // scene::TransformWS

#include <algorithm>  // std::sort
#include <cmath>      // std::sqrt, std::isfinite
#include <vector>     // std::vector (gather buffer)

namespace psynder::gameplay {

namespace {

// World-space translation packed in a TransformWS (column-major mtw).
math::Vec3 translation_of(const scene::TransformWS& t) noexcept {
    return {t.mtw.m[12], t.mtw.m[13], t.mtw.m[14]};
}

// Move `v` toward 0 by `amount` (>= 0) without crossing zero (no sign flip).
f32 decay_toward_zero(f32 v, f32 amount) noexcept {
    if (v > 0.0f) {
        const f32 r = v - amount;
        return r > 0.0f ? r : 0.0f;
    }
    if (v < 0.0f) {
        const f32 r = v + amount;
        return r < 0.0f ? r : 0.0f;
    }
    return 0.0f;
}

}  // namespace

math::Vec3 knockback_impulse(math::Vec3 target_pos, math::Vec3 source_pos,
                             f32 max_impulse, f32 radius_m) noexcept {
    if (radius_m <= 0.0f) return {0.0f, 0.0f, 0.0f};  // degenerate blast

    // Full 3D offset from the source to the target (horizontal + vertical).
    const math::Vec3 d = math::sub(target_pos, source_pos);
    const f32 dist = std::sqrt(math::dot(d, d));  // one sqrt

    // At-source degenerate case: undefined direction -> documented zero.
    if (dist <= 0.0f) return {0.0f, 0.0f, 0.0f};

    if (dist >= radius_m) return {0.0f, 0.0f, 0.0f};  // at/beyond the radius

    // Linear falloff: 1 at the source, 0 at the radius. dist is strictly in
    // (0, radius_m) here, so frac is strictly in (0, 1).
    const f32 frac = 1.0f - dist / radius_m;
    const f32 magnitude = max_impulse * frac;

    // direction = d / dist (unit); impulse = direction * magnitude.
    const f32 s = magnitude / dist;
    return {d.x * s, d.y * s, d.z * s};
}

void add_knockback(Knockback& k, math::Vec3 impulse) noexcept {
    k.velocity = math::add(k.velocity, impulse);
}

void tick_knockback(Knockback& k, f32 dt_s) noexcept {
    // Guard a non-finite or non-positive dt (a no-op).
    if (!std::isfinite(dt_s) || dt_s <= 0.0f) return;
    if (k.damping_per_s <= 0.0f) return;  // no decay configured

    const f32 amount = k.damping_per_s * dt_s;  // (m/s) bled off this tick
    k.velocity.x = decay_toward_zero(k.velocity.x, amount);
    k.velocity.y = decay_toward_zero(k.velocity.y, amount);
    k.velocity.z = decay_toward_zero(k.velocity.z, amount);
}

void apply_radial_knockback(scene::World& w, math::Vec3 source, f32 max_impulse,
                            f32 radius_m) noexcept {
    if (radius_m <= 0.0f) return;  // degenerate blast pushes nothing

    // GATHER phase: collect every Knockback victim within the radius into a
    // local buffer. We do not mutate here so the chunk-iteration order can never
    // influence the result (gather-then-mutate); the impulse is recomputed in
    // the apply phase from the same f32 inputs, so it stays bit-identical.
    std::vector<Entity> gathered;
    const f32 radius2 = radius_m * radius_m;  // squared cull bound
    w.for_each_chunk_with_entities<Knockback, scene::TransformWS>(
        [&](usize n, const Entity* ents, Knockback*, scene::TransformWS* xf) {
            for (usize i = 0; i < n; ++i) {
                const math::Vec3 c = translation_of(xf[i]);
                const math::Vec3 d = math::sub(c, source);
                const f32 dist2 = math::dot(d, d);
                if (dist2 >= radius2) continue;  // at/beyond — no push
                gathered.push_back(ents[i]);
            }
        });

    // Apply in ASCENDING entity-id order for determinism (the gather order
    // followed chunk layout; sorting pins the mutation order to entity ids).
    std::sort(gathered.begin(), gathered.end(),
              [](Entity a, Entity b) { return a.raw < b.raw; });

    for (const Entity victim : gathered) {
        Knockback* k = w.get<Knockback>(victim);
        const scene::TransformWS* xf = w.get<scene::TransformWS>(victim);
        if (k == nullptr || xf == nullptr) continue;  // defensive
        const math::Vec3 pos = translation_of(*xf);
        add_knockback(*k, knockback_impulse(pos, source, max_impulse, radius_m));
    }
}

}  // namespace psynder::gameplay
