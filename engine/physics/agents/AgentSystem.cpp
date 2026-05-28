// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/physics/agents/AgentSystem.cpp — see AgentSystem.h for the contract.
//
// Determinism strategy (recap): a GATHER walk snapshots each agent's last-tick
// position out of its TransformWS into a flat SoA (and caches the component
// write-back pointers). The READ phase then reduces the separation + static
// push-out accel SERIALLY in stable entity-id order (the spatial index mutates
// per-query scratch, so it is single-threaded by contract — Spatial.h). The
// COMPUTE+WRITE phase runs JobSystem::parallel_for over the id-sorted list: each
// job reads only the immutable snapshot + precomputed accel and writes only its
// own agent's velocity/TransformWS, so the result is independent of chunk
// evaluation order and bit-reproducible across thread counts.

#include "physics/agents/AgentSystem.h"

#include <algorithm>
#include <cmath>

#include "jobs/JobSystem.h"

namespace psynder::physics::agents {

namespace {

using math::Vec3;

// Clamp a vector's magnitude to `max_len`. Pure float ops (lane is built
// -ffp-contract=off), no fast-math reassociation.
Vec3 clamp_length(Vec3 v, f32 max_len) noexcept {
    const f32 len2 = math::dot(v, v);
    const f32 m2 = max_len * max_len;
    if (len2 > m2 && len2 > 0.0f) {
        const f32 s = max_len / std::sqrt(len2);
        return math::mul(v, s);
    }
    return v;
}

f32 vec_len(Vec3 v) noexcept { return std::sqrt(math::dot(v, v)); }

bool is_finite(Vec3 v) noexcept {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

Vec3 translation_of(const scene::TransformWS& t) noexcept {
    return {t.mtw.m[12], t.mtw.m[13], t.mtw.m[14]};
}

// FNV-1a over the static-collider AABB bytes: cheap topology fingerprint so we
// rebuild() the spatial index only when the static set changes, refit() else.
u64 fingerprint(const StaticColliders& s) noexcept {
    u64 h = 1469598103934665603ull;
    auto mix = [&h](const void* p, usize n) {
        const auto* b = static_cast<const u8*>(p);
        for (usize i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
    };
    const u64 n = s.aabbs.size();
    mix(&n, sizeof(n));
    if (!s.aabbs.empty()) mix(s.aabbs.data(), s.aabbs.size() * sizeof(math::Aabb));
    return h;
}

}  // namespace

void update_agents(scene::World& ecs, const StaticColliders& statics,
                   AgentScratch& scratch, f32 dt_seconds, const AgentTuning& tuning) {
    // ── GATHER (ECS storage order): snapshot the agent columns into a flat SoA
    //    and cache the write-back pointers. clear() keeps capacity, so after the
    //    first tick this does no heap allocation. ──────────────────────────────
    scratch.entities.clear();
    scratch.params.clear();
    scratch.read_pos.clear();
    scratch.goal.clear();
    scratch.xf_ptr.clear();
    scratch.vel_ptr.clear();
    ecs.for_each_chunk_with_entities<scene::Agent, scene::AgentVelocity,
                                     scene::AgentTarget, scene::TransformWS>(
        [&](usize cn, const Entity* ents, scene::Agent* ag, scene::AgentVelocity* vel,
            scene::AgentTarget* tgt, scene::TransformWS* xf) {
            for (usize i = 0; i < cn; ++i) {
                scratch.entities.push_back(ents[i]);
                scratch.params.push_back(ag[i]);
                scratch.read_pos.push_back(translation_of(xf[i]));
                scratch.goal.push_back(tgt[i].goal);
                scratch.xf_ptr.push_back(&xf[i]);
                scratch.vel_ptr.push_back(&vel[i]);
            }
        });

    const usize n = scratch.entities.size();
    if (n == 0) return;

    if (scratch.order.size() < n) {
        scratch.order.resize(n);
        scratch.accel.resize(n);
        scratch.agent_aabbs.resize(n);
        scratch.agent_entities.resize(n);
    }

    // ── Stable id-sorted permutation: iterate by ENTITY id, never gather/storage
    //    order (ECS swap-remove makes storage order history-dependent). ─────────
    for (usize i = 0; i < n; ++i) scratch.order[i] = static_cast<u32>(i);
    const std::vector<Entity>& ent = scratch.entities;
    std::sort(scratch.order.begin(), scratch.order.begin() + static_cast<isize>(n),
              [&ent](u32 a, u32 b) { return ent[a].raw < ent[b].raw; });

    // Build the agent self-broadphase over the id-ordered AABBs. Rank i in the
    // spatial set corresponds to gather slot scratch.order[i] (agent_entities[i]).
    for (usize k = 0; k < n; ++k) {
        const u32 slot = scratch.order[k];
        const Vec3 p = scratch.read_pos[slot];
        const f32 r = scratch.params[slot].radius_m;
        scratch.agent_aabbs[k] = math::Aabb{{p.x - r, p.y - r, p.z - r},
                                            {p.x + r, p.y + r, p.z + r}};
        scratch.agent_entities[k] = ent[slot];
    }
    scratch.agent_index.rebuild(
        std::span<const Entity>(scratch.agent_entities.data(), n),
        std::span<const math::Aabb>(scratch.agent_aabbs.data(), n));

    // Build / refit the static-collider broadphase (rebuild only on topology
    // change; cheap refit otherwise).
    const bool have_statics = !statics.entities.empty();
    if (have_statics) {
        const u64 fp = fingerprint(statics);
        const bool topo_changed = scratch.last_static_count != statics.entities.size() ||
                                  scratch.last_static_fingerprint != fp;
        if (topo_changed) {
            scratch.static_index.rebuild(statics.entities, statics.aabbs);
            scratch.last_static_count = statics.entities.size();
            scratch.last_static_fingerprint = fp;
        } else {
            scratch.static_index.refit(statics.aabbs);
        }
    }

    // Map an Entity id back to its gather slot (binary search the id-sorted set,
    // then rank -> slot via order[]).
    auto slot_of = [&](Entity e) -> u32 {
        const Entity* first = scratch.agent_entities.data();
        const Entity* last = first + n;
        const Entity* it = std::lower_bound(
            first, last, e, [](Entity a, Entity b) { return a.raw < b.raw; });
        if (it != last && it->raw == e.raw)
            return scratch.order[static_cast<usize>(it - first)];
        return 0u;
    };

    // ── READ PHASE (serial, id order): reduce separation + static push-out accel
    //    per agent. Done serially so the float sums are order-stable regardless
    //    of how the parallel compute phase is chunked. ───────────────────────────
    std::vector<Vec3>& accel = scratch.accel;
    std::vector<Entity>& qhit = scratch.qhit;
    qhit.reserve(64);

    for (usize k = 0; k < n; ++k) {
        const u32 slot = scratch.order[k];
        const Vec3 p = scratch.read_pos[slot];
        const f32 radius = scratch.params[slot].radius_m;
        Vec3 a{0.0f, 0.0f, 0.0f};

        // Separation from neighbouring agents within the perception radius.
        scratch.agent_index.query_sphere(p, tuning.perception_radius_m, qhit);
        std::sort(qhit.begin(), qhit.end(),
                  [](Entity x, Entity y) { return x.raw < y.raw; });
        const f32 desired = 2.0f * radius;  // touch distance between two agents
        for (Entity he : qhit) {
            if (he.raw == ent[slot].raw) continue;
            const u32 hs = slot_of(he);
            const Vec3 d = math::sub(p, scratch.read_pos[hs]);
            const f32 dist = vec_len(d);
            if (dist > 0.0f && dist < tuning.perception_radius_m) {
                const f32 push = (desired > 0.0f) ? (desired / dist) : 1.0f;
                a = math::add(a, math::mul(math::mul(d, 1.0f / dist), push));
            }
        }
        a = math::mul(a, tuning.separation_weight);

        // Push-out from static colliders overlapping the agent's reach.
        if (have_statics) {
            const f32 reach = radius + tuning.static_skin_m;
            scratch.static_index.query_sphere(p, reach + tuning.perception_radius_m, qhit);
            std::sort(qhit.begin(), qhit.end(),
                      [](Entity x, Entity y) { return x.raw < y.raw; });
            for (Entity he : qhit) {
                const Entity* se = statics.entities.data();
                const usize sn = statics.entities.size();
                usize si = sn;
                for (usize j = 0; j < sn; ++j)
                    if (se[j].raw == he.raw) { si = j; break; }
                if (si == sn) continue;
                const math::Aabb& box = statics.aabbs[si];
                const Vec3 cp{std::clamp(p.x, box.min.x, box.max.x),
                              std::clamp(p.y, box.min.y, box.max.y),
                              std::clamp(p.z, box.min.z, box.max.z)};
                const Vec3 d = math::sub(p, cp);
                const f32 dist = vec_len(d);
                if (dist < reach) {
                    if (dist > 1e-6f) {
                        const f32 pen = (reach - dist);
                        a = math::add(a, math::mul(math::mul(d, 1.0f / dist),
                                                   pen * tuning.separation_weight));
                    } else {
                        a = math::add(a, Vec3{0.0f, reach * tuning.separation_weight, 0.0f});
                    }
                }
            }
        }

        accel[slot] = a;
    }

    // ── COMPUTE + WRITE PHASE (parallel, order-independent) ───────────────────
    const u32* order = scratch.order.data();
    jobs::JobSystem::Get().parallel_for(
        0, n, tuning.job_grain, [&](usize lo, usize hi) {
            for (usize k = lo; k < hi; ++k) {
                const u32 slot = order[k];
                const scene::Agent& a = scratch.params[slot];
                const Vec3 p = scratch.read_pos[slot];
                const Vec3 vel0 = scratch.vel_ptr[slot]->velocity;

                // SEEK with Reynolds arrive.
                const Vec3 to_goal = math::sub(scratch.goal[slot], p);
                const f32 dist = vec_len(to_goal);
                Vec3 desired{0.0f, 0.0f, 0.0f};
                if (dist > 1e-5f) {
                    f32 speed = a.max_speed_mps;
                    if (a.arrive_radius_m > 0.0f && dist < a.arrive_radius_m)
                        speed = a.max_speed_mps * (dist / a.arrive_radius_m);
                    desired = math::mul(to_goal, speed / dist);
                }
                Vec3 seek = math::mul(math::sub(desired, vel0), tuning.seek_weight);

                Vec3 acc = math::add(seek, accel[slot]);
                acc = clamp_length(acc, a.max_force);

                // Semi-implicit Euler: v += a*dt (clamped), p += v*dt.
                Vec3 v = clamp_length(math::add(vel0, math::mul(acc, dt_seconds)),
                                      a.max_speed_mps);
                if (!is_finite(v)) v = Vec3{0.0f, 0.0f, 0.0f};
                Vec3 np = math::add(p, math::mul(v, dt_seconds));
                if (!is_finite(np)) np = p;

                scratch.vel_ptr[slot]->velocity = v;
                scene::TransformWS& t = *scratch.xf_ptr[slot];
                t.prev_mtw = t.mtw;
                t.mtw.m[12] = np.x;
                t.mtw.m[13] = np.y;
                t.mtw.m[14] = np.z;
            }
        });
}

void update_agents(scene::World& ecs, AgentScratch& scratch, f32 dt_seconds,
                   const AgentTuning& tuning) {
    update_agents(ecs, StaticColliders{}, scratch, dt_seconds, tuning);
}

}  // namespace psynder::physics::agents
