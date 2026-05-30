// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/gameplay/Weapons.cpp — see Weapons.h.

#include "gameplay/Weapons.h"

#include "gameplay/Damage.h"
#include "gameplay/GameplayComponents.h"
#include "scene/GxComponents.h"  // TransformWS

#include <algorithm>
#include <cmath>

namespace psynder::gameplay {

namespace {
math::Vec3 translation_of(const scene::TransformWS& t) noexcept {
    return {t.mtw.m[12], t.mtw.m[13], t.mtw.m[14]};
}
bool ready_and_spend(Weapon& wp) noexcept {
    if (wp.cooldown_s > 0.0f || wp.ammo == 0) return false;
    if (wp.ammo > 0) --wp.ammo;
    wp.cooldown_s = wp.fire_interval_s;
    return true;
}
}  // namespace

Entity fire_hitscan(scene::World& w, Entity shooter, math::Vec3 origin,
                    math::Vec3 dir) noexcept {
    Weapon* wp = w.get<Weapon>(shooter);
    if (wp == nullptr || !ready_and_spend(*wp)) return Entity{};

    const f32 dl = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    if (dl <= 0.0f) return Entity{};
    const math::Vec3 d{dir.x / dl, dir.y / dl, dir.z / dl};
    const f32 radius = wp->hit_radius;

    Entity best{};
    f32 best_t = 1.0e30f;
    w.for_each_chunk_with_entities<Health, scene::TransformWS>(
        [&](usize n, const Entity* ents, Health*, scene::TransformWS* xf) {
            for (usize i = 0; i < n; ++i) {
                if (ents[i].raw == shooter.raw) continue;
                const math::Vec3 c = translation_of(xf[i]);
                const math::Vec3 oc{origin.x - c.x, origin.y - c.y, origin.z - c.z};
                const f32 b = oc.x * d.x + oc.y * d.y + oc.z * d.z;
                const f32 cc =
                    oc.x * oc.x + oc.y * oc.y + oc.z * oc.z - radius * radius;
                const f32 disc = b * b - cc;
                if (disc < 0.0f) continue;
                const f32 sq = std::sqrt(disc);
                f32 t = -b - sq;
                if (t < 0.0f) t = -b + sq;
                if (t < 0.0f) continue;
                if (t < best_t || (t == best_t && ents[i].raw < best.raw)) {
                    best_t = t;
                    best = ents[i];
                }
            }
        });

    if (best.valid()) damage_credited(w, shooter, best, wp->damage);
    return best;
}

Entity fire_projectile(scene::World& w, Entity shooter, math::Vec3 origin,
                       math::Vec3 dir, f32 speed_mps, f32 ttl_s) {
    Weapon* wp = w.get<Weapon>(shooter);
    if (wp == nullptr || !ready_and_spend(*wp)) return Entity{};

    const f32 dl = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    const math::Vec3 v = (dl > 0.0f)
                             ? math::Vec3{dir.x / dl * speed_mps,
                                          dir.y / dl * speed_mps,
                                          dir.z / dl * speed_mps}
                             : math::Vec3{0.0f, 0.0f, 0.0f};
    const Entity p = w.create();
    scene::TransformWS t{};
    t.mtw = math::translate(origin);
    t.prev_mtw = t.mtw;
    w.add(p, t);
    w.add(p, Projectile{v, wp->damage, ttl_s, shooter});
    return p;
}

void tick_weapons(scene::World& w, f32 dt_seconds) {
    w.for_each_chunk<Weapon>([&](usize n, Weapon* wp) {
        for (usize i = 0; i < n; ++i) {
            wp[i].cooldown_s -= dt_seconds;
            if (wp[i].cooldown_s < 0.0f) wp[i].cooldown_s = 0.0f;
        }
    });
}

void tick_projectiles(scene::World& w, f32 dt_seconds, std::vector<Entity>& scratch) {
    // Snapshot Health targets (id + pos) once.
    struct Target { Entity e; math::Vec3 pos; };
    std::vector<Target> targets;
    w.for_each_chunk_with_entities<Health, scene::TransformWS>(
        [&](usize n, const Entity* ents, Health*, scene::TransformWS* xf) {
            for (usize i = 0; i < n; ++i)
                targets.push_back({ents[i], translation_of(xf[i])});
        });

    // Integrate projectiles in place; collect (owner, victim, damage) hits + despawns.
    struct Hit { Entity owner; Entity victim; f32 damage; };
    std::vector<Hit> hits;
    scratch.clear();
    const f32 r2 = kProjectileHitRadius * kProjectileHitRadius;
    w.for_each_chunk_with_entities<Projectile, scene::TransformWS>(
        [&](usize n, const Entity* ents, Projectile* pr, scene::TransformWS* xf) {
            for (usize i = 0; i < n; ++i) {
                xf[i].prev_mtw = xf[i].mtw;
                xf[i].mtw.m[12] += pr[i].velocity.x * dt_seconds;
                xf[i].mtw.m[13] += pr[i].velocity.y * dt_seconds;
                xf[i].mtw.m[14] += pr[i].velocity.z * dt_seconds;
                pr[i].ttl_s -= dt_seconds;
                const math::Vec3 pp = translation_of(xf[i]);
                Entity victim{};
                for (const Target& tg : targets) {
                    if (tg.e.raw == pr[i].owner.raw) continue;
                    const math::Vec3 dv{pp.x - tg.pos.x, pp.y - tg.pos.y,
                                        pp.z - tg.pos.z};
                    if (dv.x * dv.x + dv.y * dv.y + dv.z * dv.z <= r2) {
                        if (!victim.valid() || tg.e.raw < victim.raw) victim = tg.e;
                    }
                }
                if (victim.valid()) {
                    hits.push_back({pr[i].owner, victim, pr[i].damage});
                    scratch.push_back(ents[i]);  // despawn on hit
                } else if (pr[i].ttl_s <= 0.0f) {
                    scratch.push_back(ents[i]);  // despawn on ttl
                }
            }
        });

    // Apply damage + despawn in ascending entity-id order (determinism).
    std::sort(hits.begin(), hits.end(),
              [](const Hit& a, const Hit& b) { return a.victim.raw < b.victim.raw; });
    for (const Hit& h : hits) damage_credited(w, h.owner, h.victim, h.damage);
    std::sort(scratch.begin(), scratch.end(),
              [](Entity a, Entity b) { return a.raw < b.raw; });
    for (const Entity p : scratch) w.destroy(p);
}

}  // namespace psynder::gameplay
