// SPDX-License-Identifier: MIT
// Psynder-GX — interest management / area-of-interest query. Lane 18 (net).
// Issue #43, ADR-020.
//
// For 16-32 player FPS on large maps the server must not ship every entity to
// every client. Interest management answers "which entities are within a
// peer's interest sphere?" so the replication layer (#40) ships only that
// subset. This header is a SCAFFOLD — a brute-force radius query over a flat
// entity-position array.
//
// TODO(ADR-020/#43): back this with the shared engine/scene UniformGrid/Bvh
// broadphase instead of the O(n) scan, so AoI scales to thousands of entities
// without touching every one each tick.
//
// DOTS contract: EntityPos is POD / trivially-copyable / SoA-friendly. The
// query writes ids into a caller-owned vector (clear()ed then filled; reserve
// once to avoid per-frame realloc). No exceptions / RTTI in the loop.
//
// Determinism: net TUs build -fno-fast-math / -ffp-contract=off
// (cmake/HotLane.cmake, DESIGN-PSYNDER-GX.md §14). The boundary is INCLUSIVE
// (distance == radius is in-range) using a squared-distance compare so the edge
// case is bit-reproducible across platforms. Results are emitted in the input
// array order for a stable, tick-reproducible id list.

#pragma once

#include "core/Types.h"
#include "math/Math.h"

#include <span>
#include <vector>

namespace psynder::net {

// ──────────────────────────────────────────────────────────────────────────
// EntityPos — id + world position for the AoI scan. POD, trivially copyable.
// ──────────────────────────────────────────────────────────────────────────
struct EntityPos {
    u32        id = 0;
    math::Vec3 pos{0.f, 0.f, 0.f};
};

static_assert(std::is_trivially_copyable_v<EntityPos>,
              "EntityPos must be trivially copyable for the DOTS/SoA contract");

// ──────────────────────────────────────────────────────────────────────────
// aoi_query — append the ids of every entity within `radius` of `center` to
// `out_ids`. `out_ids` is cleared first then filled in input order. Boundary
// is INCLUSIVE (distance == radius -> included). Returns the in-range count.
// TODO(ADR-020/#43): swap the brute-force scan for the scene UniformGrid/Bvh
// broadphase when entity counts grow.
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

}  // namespace psynder::net
