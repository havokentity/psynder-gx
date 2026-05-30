// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/gameplay/Splash.cpp — see Splash.h.

#include "gameplay/Splash.h"

#include "gameplay/Ballistics.h"        // damage_at_distance (radial falloff)
#include "gameplay/Damage.h"            // damage_credited
#include "gameplay/GameplayComponents.h"  // Health, Team
#include "gameplay/Weapons.h"           // kNoTeam
#include "scene/GxComponents.h"         // TransformWS

#include <algorithm>  // std::sort
#include <cmath>      // std::sqrt

namespace psynder::gameplay {

namespace {
// World-space translation packed in a TransformWS (column-major mtw).
math::Vec3 translation_of(const scene::TransformWS& t) noexcept {
    return {t.mtw.m[12], t.mtw.m[13], t.mtw.m[14]};
}
}  // namespace

usize apply_splash_damage(scene::World& w, math::Vec3 center,
                          const SplashParams& p, Entity attacker,
                          i64 friendly_team, std::vector<Entity>& scratch) noexcept {
    // GATHER phase: collect every Health victim within the outer radius into the
    // reused scratch buffer. We do not apply damage here so the chunk-iteration
    // order can never influence the result (gather-then-mutate). The actual
    // damage value is recomputed in the apply phase from the same f32 inputs, so
    // it stays bit-identical regardless of when it is evaluated.
    scratch.clear();
    const f32 outer2 = p.outer_radius_m * p.outer_radius_m;  // squared cull bound
    w.for_each_chunk_with_entities<Health, scene::TransformWS>(
        [&](usize n, const Entity* ents, Health*, scene::TransformWS* xf) {
            for (usize i = 0; i < n; ++i) {
                // Friendly-team filter (mirrors fire_hitscan): when >= 0, spare a
                // victim carrying that exact team. kNoTeam (-1) disables it.
                if (friendly_team >= 0) {
                    const Team* tm = w.get<Team>(ents[i]);
                    if (tm != nullptr &&
                        tm->team == static_cast<u32>(friendly_team)) {
                        continue;
                    }
                }
                // Spherical blast: full 3D distance from the detonation point.
                const math::Vec3 c = translation_of(xf[i]);
                const math::Vec3 d{c.x - center.x, c.y - center.y, c.z - center.z};
                const f32 dist2 = d.x * d.x + d.y * d.y + d.z * d.z;
                if (dist2 > outer2) continue;  // beyond the blast — untouched
                scratch.push_back(ents[i]);
            }
        });

    // Apply in ASCENDING entity-id order for determinism (the gather order
    // followed chunk layout; sorting pins the mutation order to entity ids).
    std::sort(scratch.begin(), scratch.end(),
              [](Entity a, Entity b) { return a.raw < b.raw; });

    usize hit_count = 0;
    for (const Entity victim : scratch) {
        // Re-read the position (the entity still exists; a kill only tags Dead,
        // it does not remove the TransformWS) and recompute the falloff damage.
        const scene::TransformWS* xf = w.get<scene::TransformWS>(victim);
        if (xf == nullptr) continue;  // defensive: skip if it lost its transform
        const math::Vec3 c = translation_of(*xf);
        const math::Vec3 d{c.x - center.x, c.y - center.y, c.z - center.z};
        const f32 dist = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);

        // Inner radius = full damage; outer radius = zero (min_fraction 0).
        const f32 dmg = damage_at_distance(p.max_damage, dist, p.inner_radius_m,
                                           p.outer_radius_m, 0.0f);
        if (dmg <= 0.0f) continue;  // rounds to nothing (e.g. exactly at outer)

        damage_credited(w, attacker, victim, dmg);
        ++hit_count;
    }
    return hit_count;
}

}  // namespace psynder::gameplay
