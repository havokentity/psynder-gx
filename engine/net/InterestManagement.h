// SPDX-License-Identifier: MIT
// Psynder-GX — interest management / area-of-interest query. Lane 18 (net).
// Issue #43, ADR-020.
//
// For 16-32 player FPS on large maps the server must not ship every entity to
// every client. Interest management answers "which entities are within a
// peer's interest sphere?" so the replication layer (#40) ships only that
// subset.
//
// This used to be a brute-force O(n) radius scan over a flat EntityPos array.
// It is now backed by the shared engine/scene SpatialIndex broadphase
// (UniformGrid / Bvh router) so AoI scales to thousands of entities without
// touching every one each tick. The brute-force `aoi_query` free function is
// retained UNCHANGED as the determinism oracle the unit tests validate the
// broadphase against (parity must be byte-identical).
//
// DOTS contract: EntityPos is POD / trivially-copyable / SoA-friendly. Queries
// write ids into a caller-owned vector (clear()ed then filled). The InterestSet
// reuses a persistent SpatialIndex + scratch buffers across calls — no per-query
// heap allocation in the hot path. No exceptions / RTTI in the loop.
//
// Determinism: net TUs build -fno-fast-math / -ffp-contract=off
// (cmake/HotLane.cmake, DESIGN-PSYNDER-GX.md §14). The boundary is INCLUSIVE
// (distance == radius is in-range) using a squared-distance compare so the edge
// case is bit-reproducible across platforms — the broadphase honours the same
// convention because the entity proxies are DEGENERATE (zero-extent) AABBs, so
// the scene `aabb_dist2_to_point` predicate reduces to an exact point-to-point
// squared distance identical to the brute-force scan. AoI output is sorted by
// entity id, so the visible set is order-stable for snapshot composition
// regardless of the broadphase's internal traversal order.

#pragma once

#include "core/Types.h"
#include "math/Math.h"
#include "scene/Spatial.h"

#include <algorithm>
#include <span>
#include <vector>

namespace psynder::net {

// ──────────────────────────────────────────────────────────────────────────
// EntityPos — id + world position for the AoI set. POD, trivially copyable.
// ──────────────────────────────────────────────────────────────────────────
struct EntityPos {
    u32        id = 0;
    math::Vec3 pos{0.f, 0.f, 0.f};
};

static_assert(std::is_trivially_copyable_v<EntityPos>,
              "EntityPos must be trivially copyable for the DOTS/SoA contract");

// ──────────────────────────────────────────────────────────────────────────
// aoi_query (brute force) — append the ids of every entity within `radius` of
// `center` to `out_ids`. `out_ids` is cleared first then filled in input
// order. Boundary is INCLUSIVE (distance == radius -> included). Returns the
// in-range count. Retained as the broadphase oracle: cheap, obviously correct,
// O(n). For large entity counts use InterestSet (below) instead.
// ──────────────────────────────────────────────────────────────────────────
inline usize aoi_query(math::Vec3 center, f32 radius,
                       std::span<const EntityPos> all,
                       std::vector<u32>&          out_ids) noexcept {
    out_ids.clear();
    const f32 radius_sq = radius * radius;
    for (const EntityPos& e : all) {
        const math::Vec3 d = math::sub(e.pos, center);
        if (math::dot(d, d) <= radius_sq) out_ids.push_back(e.id);
    }
    return out_ids.size();
}

// ──────────────────────────────────────────────────────────────────────────
// InterestSet — broadphase-backed area-of-interest over a persistent entity
// set. Build the set once (or whenever the entity roster changes) with build()
// / rebuild(); update positions in place every tick with refit(); then run any
// number of aoi_query() sphere queries against the shared SpatialIndex.
//
// Id bridging: the net layer uses a u32 id while SpatialIndex hands back
// psynder::Entity. We pack the u32 id into Entity.raw verbatim (low 32 bits),
// so unpacking is a plain .index() with zero lookup. This keeps the proxy set
// POD and the query path allocation-free (the persistent index + reused scratch
// vectors absorb all the churn).
//
// NOT thread-safe: the underlying SpatialIndex grid query mutates per-proxy
// scratch, so a single InterestSet must be queried from one thread at a time.
// ──────────────────────────────────────────────────────────────────────────
class InterestSet {
public:
    InterestSet() noexcept = default;

    usize size()  const noexcept { return entities_.size(); }
    bool  empty() const noexcept { return entities_.empty(); }

    // Topology path — (re)build the proxy set from `all`. Call when the entity
    // roster (count or id ordering) changes. Reuses internal storage.
    void rebuild(std::span<const EntityPos> all) {
        entities_.clear();
        aabbs_.clear();
        entities_.reserve(all.size());
        aabbs_.reserve(all.size());
        for (const EntityPos& e : all) {
            entities_.push_back(Entity{static_cast<u64>(e.id)});
            aabbs_.push_back(point_aabb(e.pos));
        }
        index_.rebuild(std::span<const Entity>(entities_.data(), entities_.size()),
                       std::span<const math::Aabb>(aabbs_.data(), aabbs_.size()));
    }

    // Cheap per-tick path — same roster (count + order) as the last rebuild(),
    // only positions moved. Refits the broadphase in place.
    void refit(std::span<const EntityPos> all) {
        if (all.size() != aabbs_.size()) {
            rebuild(all);  // roster changed under us — fall back to a full rebuild.
            return;
        }
        for (usize i = 0; i < all.size(); ++i) aabbs_[i] = point_aabb(all[i].pos);
        index_.refit(std::span<const math::Aabb>(aabbs_.data(), aabbs_.size()));
    }

    // Query path — append the ids within `radius` of `center` to `out_ids`,
    // SORTED ascending by id so the visible set is deterministic and order-
    // stable for snapshot composition. Boundary INCLUSIVE (distance == radius
    // -> included), matching aoi_query(). Returns the in-range count. No heap
    // allocation beyond `out_ids` growth (the index scratch is reused).
    usize aoi_query(math::Vec3 center, f32 radius, std::vector<u32>& out_ids) {
        out_ids.clear();
        if (index_.empty()) return 0;
        index_.query_sphere(center, radius, query_scratch_);
        out_ids.reserve(query_scratch_.size());
        for (const Entity& e : query_scratch_) out_ids.push_back(e.index());
        std::sort(out_ids.begin(), out_ids.end());
        return out_ids.size();
    }

private:
    // A degenerate (zero-extent) box centred on the entity. min == max == pos,
    // so scene::aabb_dist2_to_point reduces to an exact point-to-point squared
    // distance — byte-identical to the brute-force scan's compare, boundary and
    // all. No epsilon padding (which would bias the inclusive radius test).
    static math::Aabb point_aabb(math::Vec3 p) noexcept { return math::Aabb{p, p}; }

    scene::SpatialIndex   index_;
    std::vector<Entity>   entities_;      // proxy ids (u32 packed into Entity.raw)
    std::vector<math::Aabb> aabbs_;       // persistent proxy boxes, refit in place
    std::vector<Entity>   query_scratch_; // reused per-query result buffer
};

}  // namespace psynder::net
