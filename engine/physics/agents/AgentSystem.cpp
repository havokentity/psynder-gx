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
    const u32 kContacts = tuning.max_static_contacts;
    if (kContacts > 0) {
        if (scratch.near_static.size() < n * kContacts)
            scratch.near_static.resize(n * kContacts);
        std::fill(scratch.near_static.begin(),
                  scratch.near_static.begin() + static_cast<isize>(n * kContacts), ~0u);
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

        accel[slot] = a;  // agent-agent separation only

        // Record nearby static colliders for the write-phase HARD non-penetration
        // resolve. A soft push-out force can't beat a strong seek (agents pressed
        // through walls), so statics are resolved at the position level instead.
        // The index query mutates per-query scratch (single-threaded by contract),
        // so we gather the candidates here, serially, and resolve in parallel.
        if (have_statics && kContacts > 0) {
            const f32 record_r =
                radius + tuning.static_skin_m + tuning.perception_radius_m;
            scratch.static_index.query_sphere(p, record_r, qhit);
            std::sort(qhit.begin(), qhit.end(),
                      [](Entity x, Entity y) { return x.raw < y.raw; });
            u32 stored = 0;
            for (Entity he : qhit) {
                if (stored >= kContacts) break;
                const Entity* se = statics.entities.data();
                const usize sn = statics.entities.size();
                usize si = sn;
                for (usize j = 0; j < sn; ++j)
                    if (se[j].raw == he.raw) { si = j; break; }
                if (si == sn) continue;
                scratch.near_static[slot * kContacts + stored] = static_cast<u32>(si);
                ++stored;
            }
        }
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
                if (tuning.planar) acc.y = 0.0f;  // ground crowd: no vertical steering
                acc = clamp_length(acc, a.max_force);

                // Semi-implicit Euler: v += a*dt (clamped), p += v*dt.
                Vec3 v = clamp_length(math::add(vel0, math::mul(acc, dt_seconds)),
                                      a.max_speed_mps);
                if (tuning.planar) v.y = 0.0f;
                if (!is_finite(v)) v = Vec3{0.0f, 0.0f, 0.0f};
                Vec3 np = math::add(p, math::mul(v, dt_seconds));
                if (tuning.planar) np.y = tuning.ground_y + a.radius_m;
                if (!is_finite(np)) np = p;

                // HARD non-penetration vs the recorded static colliders: push the
                // agent centre out of each box expanded by (radius + skin), along
                // the axis of least penetration, and kill the velocity into that
                // face. Guarantees the agent never ends a tick inside / through a
                // wall regardless of how hard seek pulled it in. Deterministic:
                // boxes are processed in the stored (id-sorted) order; pure math,
                // no shared state, so it is chunk/thread-order independent.
                if (have_statics && kContacts > 0) {
                    const f32 reach = a.radius_m + tuning.static_skin_m;
                    for (u32 c = 0; c < kContacts; ++c) {
                        const u32 si = scratch.near_static[slot * kContacts + c];
                        if (si == ~0u) break;
                        const math::Aabb& box = statics.aabbs[si];
                        const f32 lo[3] = {box.min.x - reach, box.min.y - reach,
                                           box.min.z - reach};
                        const f32 hi[3] = {box.max.x + reach, box.max.y + reach,
                                           box.max.z + reach};
                        f32 cc[3] = {np.x, np.y, np.z};
                        const bool inside = cc[0] > lo[0] && cc[0] < hi[0] &&
                                            cc[1] > lo[1] && cc[1] < hi[1] &&
                                            cc[2] > lo[2] && cc[2] < hi[2];
                        if (!inside) continue;
                        int best = -1;
                        f32 best_pen = 1e30f, best_sign = 1.0f;
                        for (int ax = 0; ax < 3; ++ax) {
                            if (tuning.planar && ax == 1) continue;  // ground clamp owns Y
                            const f32 pen_lo = cc[ax] - lo[ax];
                            const f32 pen_hi = hi[ax] - cc[ax];
                            const f32 pen = pen_lo < pen_hi ? pen_lo : pen_hi;
                            if (pen < best_pen) {
                                best_pen = pen;
                                best = ax;
                                best_sign = pen_lo < pen_hi ? -1.0f : 1.0f;
                            }
                        }
                        if (best < 0) continue;
                        cc[best] += best_sign * best_pen;
                        np = Vec3{cc[0], cc[1], cc[2]};
                        f32 vv[3] = {v.x, v.y, v.z};
                        if (vv[best] * best_sign < 0.0f) vv[best] = 0.0f;  // stop into-wall
                        v = Vec3{vv[0], vv[1], vv[2]};
                    }
                    if (tuning.planar) np.y = tuning.ground_y + a.radius_m;
                }

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
