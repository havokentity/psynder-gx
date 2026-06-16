// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/gameplay/Grenade.cpp — see Grenade.h.

#include "gameplay/Grenade.h"

#include "gameplay/Splash.h"  // apply_splash_damage
#include "scene/GxComponents.h"  // TransformWS

#include <algorithm>
#include <cmath>
#include <vector>

namespace psynder::gameplay {

namespace {
// The grenade's world position lives in its TransformWS translation column
// (m[12..14]), mirroring Weapons.cpp's projectile integration.
math::Vec3 translation_of(const scene::TransformWS& t) noexcept {
    return {t.mtw.m[12], t.mtw.m[13], t.mtw.m[14]};
}
}  // namespace

Entity throw_grenade(scene::World& w, Entity thrower, i64 thrower_team,
                     math::Vec3 origin, math::Vec3 dir, f32 speed_mps,
                     f32 fuse_s, const SplashParams& blast) {
    // Normalize the throw direction (one sqrt; no transcendentals). A zero-length
    // dir has no heading, so there is nothing to throw.
    const f32 dl = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    if (dl <= 0.0f) return Entity{};
    const math::Vec3 v{dir.x / dl * speed_mps, dir.y / dl * speed_mps,
                       dir.z / dl * speed_mps};

    const Entity g = w.create();
    scene::TransformWS t{};
    t.mtw = math::translate(origin);
    t.prev_mtw = t.mtw;
    w.add(g, t);
    w.add(g, Grenade{v, fuse_s, blast, thrower, thrower_team});
    return g;
}

void tick_grenades(scene::World& w, f32 dt_seconds, std::vector<Entity>& scratch) {
    // A pending detonation, captured at the grenade's integrated position so the
    // later apply pass is independent of iteration / spawn order.
    struct Blast {
        Entity       e;       // the spent grenade (for ascending-id ordering)
        math::Vec3   center;  // detonation point, world space
        SplashParams params;  // blast shape
        Entity       owner;   // credited the kills
        i64          team;    // friendly filter
    };
    std::vector<Blast> blasts;

    // Integrate every grenade in place (gravity -> velocity -> position), tick
    // the fuse, and gather the ones that detonate this step. The despawn list is
    // accumulated in the reused `scratch` (no per-tick heap growth).
    scratch.clear();
    w.for_each_chunk_with_entities<Grenade, scene::TransformWS>(
        [&](usize n, const Entity* ents, Grenade* gr, scene::TransformWS* xf) {
            for (usize i = 0; i < n; ++i) {
                // Gravity pulls the vertical velocity down (m/s² * s = m/s).
                gr[i].velocity.y -= kGrenadeGravity * dt_seconds;
                // Integrate the world position by the new velocity.
                xf[i].prev_mtw = xf[i].mtw;
                xf[i].mtw.m[12] += gr[i].velocity.x * dt_seconds;
                xf[i].mtw.m[13] += gr[i].velocity.y * dt_seconds;
                xf[i].mtw.m[14] += gr[i].velocity.z * dt_seconds;
                // Burn the fuse; detonate when it reaches zero.
                gr[i].fuse_s -= dt_seconds;
                if (gr[i].fuse_s <= 0.0f) {
                    blasts.push_back({ents[i], translation_of(xf[i]), gr[i].blast,
                                      gr[i].owner, gr[i].owner_team});
                    scratch.push_back(ents[i]);  // despawn after the pass
                }
            }
        });

    // Apply detonations + despawn in ascending entity-id order, so the order in
    // which grenades were iterated / spawned cannot affect the resulting damage.
    std::sort(blasts.begin(), blasts.end(),
              [](const Blast& a, const Blast& b) { return a.e.raw < b.e.raw; });
    std::vector<Entity> victims;  // apply_splash_damage's reused gather buffer
    for (const Blast& b : blasts)
        apply_splash_damage(w, b.center, b.params, b.owner, b.team, victims);

    std::sort(scratch.begin(), scratch.end(),
              [](Entity a, Entity b) { return a.raw < b.raw; });
    for (const Entity g : scratch) w.destroy(g);
}

}  // namespace psynder::gameplay
