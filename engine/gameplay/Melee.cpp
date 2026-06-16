// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/gameplay/Melee.cpp — see Melee.h.

#include "gameplay/Melee.h"

#include "gameplay/Damage.h"             // damage_credited
#include "gameplay/GameplayComponents.h" // Health, Team
#include "scene/GxComponents.h"          // TransformWS

#include <cmath>

namespace psynder::gameplay {

namespace {

// World-space translation column of a baked column-major model-to-world matrix.
math::Vec3 translation_of(const scene::TransformWS& t) noexcept {
    return {t.mtw.m[12], t.mtw.m[13], t.mtw.m[14]};
}

}  // namespace

Entity melee_attack(scene::World& w, Entity attacker, math::Vec3 origin,
                    math::Vec3 facing, f32 range_m, f32 cos_half_angle,
                    f32 damage, i64 friendly_team) noexcept {
    // Guard degenerate range: a non-positive reach can hit nothing.
    if (range_m <= 0.0f) return Entity{};

    // Guard degenerate facing: a zero-length axis has no cone direction.
    const f32 fl = std::sqrt(facing.x * facing.x + facing.y * facing.y +
                             facing.z * facing.z);
    if (fl <= 0.0f) return Entity{};
    const math::Vec3 f{facing.x / fl, facing.y / fl, facing.z / fl};

    const f32 range_sq = range_m * range_m;

    Entity best{};
    f32 best_dist_sq = 1.0e30f;
    w.for_each_chunk_with_entities<Health, scene::TransformWS>(
        [&](usize n, const Entity* ents, Health* hp, scene::TransformWS* xf) {
            for (usize i = 0; i < n; ++i) {
                // Never hit yourself.
                if (ents[i].raw == attacker.raw) continue;
                // Only LIVING enemies are valid targets.
                if (hp[i].hp <= 0.0f) continue;
                // Team-aware filter: pass over teammates (the enemy behind a
                // teammate can still be struck), mirroring fire_hitscan.
                if (friendly_team >= 0) {
                    const Team* tm = w.get<Team>(ents[i]);
                    if (tm != nullptr &&
                        tm->team == static_cast<u32>(friendly_team)) {
                        continue;
                    }
                }

                const math::Vec3 c = translation_of(xf[i]);
                const math::Vec3 to{c.x - origin.x, c.y - origin.y,
                                    c.z - origin.z};
                const f32 dist_sq = to.x * to.x + to.y * to.y + to.z * to.z;

                // Outside reach: distance to the centre must be <= range_m.
                if (dist_sq > range_sq) continue;
                // Degenerate: a target exactly at the origin has no direction to
                // test against the cone — skip it rather than divide by zero.
                if (dist_sq <= 0.0f) continue;

                // Inside the cone? Compare the cosine of the angle between the
                // facing axis and the (normalized) direction to the target
                // against the precomputed cosine threshold. No runtime trig.
                const f32 inv = 1.0f / std::sqrt(dist_sq);
                const f32 cos_to =
                    (to.x * f.x + to.y * f.y + to.z * f.z) * inv;
                if (cos_to < cos_half_angle) continue;

                // Nearest qualifier wins; ascending-id tie-break for determinism.
                if (dist_sq < best_dist_sq ||
                    (dist_sq == best_dist_sq && ents[i].raw < best.raw)) {
                    best_dist_sq = dist_sq;
                    best = ents[i];
                }
            }
        });

    if (best.valid()) damage_credited(w, attacker, best, damage);
    return best;
}

f32 melee_cone_cos(f32 half_angle_deg) noexcept {
    // SETUP-ONLY: std::cos is fine at authoring time. Never call per-tick.
    constexpr f32 kPi = 3.14159265358979323846f;
    return std::cos(half_angle_deg * (kPi / 180.0f));
}

}  // namespace psynder::gameplay
