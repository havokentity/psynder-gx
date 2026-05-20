// SPDX-License-Identifier: MIT
// Psynder — Lane 06 spatial index module (M2/M3).
//
// A self-contained set of broad-phase spatial accelerators over a flat array
// of world-space AABB proxies, plus a query router that picks/combines them:
//
//   * Bvh         — refittable bounding-volume hierarchy (median split). Cheap
//                   refit when transforms move, full rebuild on topology change.
//   * Sap         — sweep-and-prune broadphase (overlapping-pair generation).
//   * UniformGrid — hashed uniform grid for sphere / point / small-AABB queries.
//   * SpatialIndex— router that owns the proxy set and routes ray / frustum /
//                   sphere / AABB queries plus pair generation to the best
//                   structure, refitting the BVH and lazily rebuilding the
//                   SAP / grid as proxies move.
//
// This is NOT a frozen Public*.h contract — it is a normal lane-06 header that
// may evolve. Build / refit parallelise over psynder::jobs::JobSystem.
//
// Coding rules (DESIGN §3 + AGENTS.md): POD-friendly, no virtual / RTTI /
// exceptions in the hot path, no std::shared_ptr. Real metric units throughout
// (1 world unit = 1 metre); cell sizes, radii and ray lengths are all metres.
//
// The accelerators and the brute-force baseline share ONE set of intersection
// predicates (below), so an accelerated query and the linear reference produce
// byte-identical result sets by construction — that is what the unit tests
// assert over ~100k proxies.

#pragma once

#include <algorithm>
#include <cmath>
#include <limits>
#include <span>
#include <utility>
#include <vector>

#include "core/Types.h"
#include "math/Math.h"

namespace psynder::scene {

// ─── Query primitives ────────────────────────────────────────────────────────

// A ray with a finite extent. `dir` need not be unit length; `tmax` is measured
// in units of `dir` (use a unit `dir` to make `tmax` a distance in metres).
struct Ray {
    math::Vec3 origin{0.0f, 0.0f, 0.0f};
    math::Vec3 dir{1.0f, 0.0f, 0.0f};
    f32 tmax = std::numeric_limits<f32>::max();
};

// Half-space plane. A point p is on the inside when dot(n, p) + d >= 0.
struct Plane {
    math::Vec3 n{0.0f, 1.0f, 0.0f};
    f32 d = 0.0f;
};

// Six-plane convex frustum, all normals pointing inward.
struct Frustum {
    Plane planes[6];
};

// ─── AABB helpers ────────────────────────────────────────────────────────────

// An "empty" box: min = +inf, max = -inf, so merging any real box yields that
// box. Used as the identity element of aabb_merge reductions.
PSY_FORCEINLINE math::Aabb aabb_empty() noexcept {
    constexpr f32 kInf = std::numeric_limits<f32>::infinity();
    return math::Aabb{{kInf, kInf, kInf}, {-kInf, -kInf, -kInf}};
}

PSY_FORCEINLINE math::Aabb aabb_merge(const math::Aabb& a, const math::Aabb& b) noexcept {
    return math::Aabb{
        {std::min(a.min.x, b.min.x), std::min(a.min.y, b.min.y), std::min(a.min.z, b.min.z)},
        {std::max(a.max.x, b.max.x), std::max(a.max.y, b.max.y), std::max(a.max.z, b.max.z)}};
}

PSY_FORCEINLINE math::Vec3 aabb_center(const math::Aabb& b) noexcept {
    return {0.5f * (b.min.x + b.max.x), 0.5f * (b.min.y + b.max.y), 0.5f * (b.min.z + b.max.z)};
}

PSY_FORCEINLINE math::Vec3 aabb_extent(const math::Aabb& b) noexcept {
    return {b.max.x - b.min.x, b.max.y - b.min.y, b.max.z - b.min.z};
}

// Component accessor by axis index (0=x, 1=y, 2=z) without UB pointer punning.
PSY_FORCEINLINE f32 axis(math::Vec3 v, u32 a) noexcept {
    return a == 0u ? v.x : (a == 1u ? v.y : v.z);
}

// ─── Shared intersection predicates ──────────────────────────────────────────
// Single source of truth: every accelerator's leaf test AND the brute-force
// baseline call these, so their result sets match exactly.

PSY_FORCEINLINE bool aabb_overlaps_aabb(const math::Aabb& a, const math::Aabb& b) noexcept {
    return a.min.x <= b.max.x && a.max.x >= b.min.x && a.min.y <= b.max.y && a.max.y >= b.min.y &&
           a.min.z <= b.max.z && a.max.z >= b.min.z;
}

PSY_FORCEINLINE bool aabb_contains_point(const math::Aabb& b, math::Vec3 p) noexcept {
    return p.x >= b.min.x && p.x <= b.max.x && p.y >= b.min.y && p.y <= b.max.y && p.z >= b.min.z &&
           p.z <= b.max.z;
}

// Squared distance from a point to the closest point on the box (0 if inside).
PSY_FORCEINLINE f32 aabb_dist2_to_point(const math::Aabb& b, math::Vec3 p) noexcept {
    const f32 dx = p.x < b.min.x ? b.min.x - p.x : (p.x > b.max.x ? p.x - b.max.x : 0.0f);
    const f32 dy = p.y < b.min.y ? b.min.y - p.y : (p.y > b.max.y ? p.y - b.max.y : 0.0f);
    const f32 dz = p.z < b.min.z ? b.min.z - p.z : (p.z > b.max.z ? p.z - b.max.z : 0.0f);
    return dx * dx + dy * dy + dz * dz;
}

PSY_FORCEINLINE bool aabb_intersects_sphere(const math::Aabb& b, math::Vec3 c, f32 r) noexcept {
    return aabb_dist2_to_point(b, c) <= r * r;
}

// Branchless slab test against a precomputed reciprocal direction. Uses
// fmin/fmax (not std::min/std::max) so a zero direction component — whose
// reciprocal is +/-inf — is handled robustly: fmin/fmax discard the NaN that a
// 0*inf product would yield on a slab boundary, so the slab simply imposes no
// constraint instead of poisoning the result. Axis-aligned rays are fine.
PSY_FORCEINLINE bool aabb_intersects_ray(const math::Aabb& b,
                                         math::Vec3 origin,
                                         math::Vec3 inv_dir,
                                         f32 tmax) noexcept {
    const f32 tx1 = (b.min.x - origin.x) * inv_dir.x;
    const f32 tx2 = (b.max.x - origin.x) * inv_dir.x;
    f32 tmin = std::fmin(tx1, tx2);
    f32 texit = std::fmax(tx1, tx2);

    const f32 ty1 = (b.min.y - origin.y) * inv_dir.y;
    const f32 ty2 = (b.max.y - origin.y) * inv_dir.y;
    tmin = std::fmax(tmin, std::fmin(ty1, ty2));
    texit = std::fmin(texit, std::fmax(ty1, ty2));

    const f32 tz1 = (b.min.z - origin.z) * inv_dir.z;
    const f32 tz2 = (b.max.z - origin.z) * inv_dir.z;
    tmin = std::fmax(tmin, std::fmin(tz1, tz2));
    texit = std::fmin(texit, std::fmax(tz1, tz2));

    return texit >= std::fmax(tmin, 0.0f) && tmin <= tmax;
}

PSY_FORCEINLINE math::Vec3 ray_inv_dir(const Ray& ray) noexcept {
    return {1.0f / ray.dir.x, 1.0f / ray.dir.y, 1.0f / ray.dir.z};
}

// True when the box lies entirely on the negative side of the plane: even the
// box vertex farthest along +n is still outside, so the box can be culled.
PSY_FORCEINLINE bool aabb_outside_plane(const math::Aabb& b, const Plane& pl) noexcept {
    const math::Vec3 pv{pl.n.x >= 0.0f ? b.max.x : b.min.x,
                        pl.n.y >= 0.0f ? b.max.y : b.min.y,
                        pl.n.z >= 0.0f ? b.max.z : b.min.z};
    return math::dot(pl.n, pv) + pl.d < 0.0f;
}

// Conservative AABB-vs-frustum: keep the box unless it is fully outside one of
// the six planes. (The classic p-vertex test can admit a few boxes in corner
// regions, but the brute-force baseline applies the identical predicate.)
PSY_FORCEINLINE bool aabb_in_frustum(const math::Aabb& b, const Frustum& fr) noexcept {
    for (const Plane& pl : fr.planes) {
        if (aabb_outside_plane(b, pl))
            return false;
    }
    return true;
}

// ─── Refittable BVH ──────────────────────────────────────────────────────────
//
// Median-split binary BVH. Each split partitions the primitive range into two
// equal-count halves by position (std::nth_element on the longest centroid
// axis), so the tree is balanced — depth <= ceil(log2(N)) + 1 regardless of
// geometry — which keeps the iterative traversal stack a small fixed array.
//
// The node-bounds pass is shared by build() and refit(): leaves recompute from
// the (borrowed) primitive AABBs, internal nodes merge their two children, all
// processed level-by-level deepest-first so each level parallelises cleanly
// over the job system with disjoint writes.
//
// Lifetime: build()/refit() borrow the caller's AABB array (a std::span) and
// queries read it. Keep the array alive and update it in place (same length,
// same order) before calling refit(); a length/order change needs build().
class Bvh {
   public:
    struct Node {
        math::Aabb bounds{};
        u32 first = 0;  ///< leaf: first slot in order(); internal: left child node index
        u32 count = 0;  ///< leaf: primitive count (>0); internal: 0
        u32 depth = 0;  ///< distance from root; drives the refit level grouping
    };

    void build(std::span<const math::Aabb> aabbs, u32 leaf_size = 4);
    void refit(std::span<const math::Aabb> aabbs);
    void clear() noexcept;

    bool empty() const noexcept { return nodes_.empty(); }
    u32 node_count() const noexcept { return static_cast<u32>(nodes_.size()); }
    u32 max_depth() const noexcept { return max_depth_; }

    // Root (scene) bounds; an empty box when the tree has no primitives.
    math::Aabb bounds() const noexcept;

    void query_ray(const Ray& ray, std::vector<u32>& out) const;
    void query_frustum(const Frustum& fr, std::vector<u32>& out) const;
    void query_aabb(const math::Aabb& box, std::vector<u32>& out) const;
    void query_sphere(math::Vec3 center, f32 radius, std::vector<u32>& out) const;

   private:
    void build_into(u32 self, u32 first, u32 count, u32 depth, const std::vector<math::Vec3>& centroids);
    void build_levels();
    u32 alloc_node();

    std::span<const math::Aabb> prims_{};
    std::vector<Node> nodes_;
    std::vector<u32> order_;           ///< primitive-index permutation
    std::vector<u32> nodes_by_depth_;  ///< node indices grouped by depth, deepest first
    std::vector<u32> level_offsets_;   ///< group boundaries into nodes_by_depth_
    u32 leaf_size_ = 4;
    u32 max_depth_ = 0;
};

// ─── Sweep-and-prune broadphase ──────────────────────────────────────────────
//
// Projects every proxy onto the axis of greatest centre variance and sorts by
// interval start. overlap_pairs() sweeps the sorted order keeping an active set
// of still-open intervals, emitting canonical (i<j) pairs that pass the full
// 3-axis AABB test. query_aabb() binary-searches the sorted axis to prune the
// far half before the full test. The sweep is inherently sequential; the
// projection precompute parallelises over the job system.
class Sap {
   public:
    void build(std::span<const math::Aabb> aabbs);
    void rebuild();  // re-project + re-sort the borrowed AABBs after a refit
    void clear() noexcept;

    bool empty() const noexcept { return prims_.empty(); }
    u32 sweep_axis() const noexcept { return axis_; }

    void overlap_pairs(std::vector<std::pair<u32, u32>>& out) const;
    void query_aabb(const math::Aabb& box, std::vector<u32>& out) const;

   private:
    std::span<const math::Aabb> prims_{};
    std::vector<u32> sorted_;  ///< proxy indices sorted by min on axis_
    u32 axis_ = 0;
};

// ─── Hashed uniform grid ─────────────────────────────────────────────────────
//
// Open-addressing spatial hash from integer cell coordinates to a contiguous
// slice of a flat entry array (count-then-fill, no per-cell heap churn). A
// proxy is registered in every cell its AABB overlaps, so it is expected to be
// small relative to the cell size (the router sizes cells from the proxy
// extents). Queries gather candidate cells, apply the shared predicate, and
// de-duplicate proxies that straddle multiple cells via a per-query epoch
// stamp. Query methods mutate that scratch, so they are not const and a single
// grid instance must not be queried from multiple threads concurrently.
class UniformGrid {
   public:
    void build(std::span<const math::Aabb> aabbs, f32 cell_size);
    void rebuild();  // re-bin the borrowed AABBs at the same cell size
    void clear() noexcept;

    bool empty() const noexcept { return prims_.empty(); }
    f32 cell_size() const noexcept { return cell_; }
    u32 cell_count() const noexcept { return occupied_; }

    void query_sphere(math::Vec3 center, f32 radius, std::vector<u32>& out);
    void query_aabb(const math::Aabb& box, std::vector<u32>& out);
    void query_point(math::Vec3 p, std::vector<u32>& out);

   private:
    struct Cell {
        i32 x = 0, y = 0, z = 0;
        u32 start = 0;  ///< first slot in entries_
        u32 count = 0;  ///< number of proxies in this cell
    };

    void rebuild_internal();
    i32 cell_coord(f32 v) const noexcept;
    usize find_slot(i32 cx, i32 cy, i32 cz) const noexcept;  // probe an occupied table
    u32& visit_stamp(u32 proxy) noexcept { return visit_[proxy]; }

    std::span<const math::Aabb> prims_{};
    std::vector<Cell> table_;   ///< open-addressing hash table (power-of-two)
    std::vector<u8> used_;      ///< occupancy flags parallel to table_
    std::vector<u32> entries_;  ///< proxy indices grouped by cell
    std::vector<u32> visit_;    ///< per-proxy epoch stamp for query dedup
    u32 epoch_ = 0;
    u32 occupied_ = 0;
    u32 mask_ = 0;  ///< table_.size() - 1
    f32 cell_ = 1.0f;
    f32 inv_cell_ = 1.0f;
};

// ─── Query router ────────────────────────────────────────────────────────────
//
// Owns the proxy set (entities + AABBs) and the three accelerators. rebuild()
// is the topology path (entity set / order changed); refit() is the cheap
// per-frame path (same proxies, new transforms) — it refits the BVH and marks
// the SAP / grid dirty so they rebuild lazily on the next query that needs
// them. Routing: ray and frustum -> BVH; sphere and AABB -> grid for compact
// queries, BVH for large ones; overlapping pairs -> SAP.
class SpatialIndex {
   public:
    enum class Backend : u8 { Bvh, Grid, Sap };

    struct Params {
        u32 bvh_leaf_size = 4;
        f32 cell_size = 0.0f;  ///< 0 = derive from proxy extents
    };

    void rebuild(std::span<const Entity> entities, std::span<const math::Aabb> aabbs, Params params);
    // Default-params overload. The body is a complete-class context, so Params'
    // default member initializers are usable here (a `= {}` default argument is
    // not, since the enclosing class is still incomplete at that point).
    void rebuild(std::span<const Entity> entities, std::span<const math::Aabb> aabbs) {
        rebuild(entities, aabbs, Params{});
    }
    void refit(std::span<const math::Aabb> aabbs);  // same count / order as rebuild()
    void clear() noexcept;

    usize size() const noexcept { return entities_.size(); }
    bool empty() const noexcept { return entities_.empty(); }
    math::Aabb scene_bounds() const noexcept { return scene_bounds_; }
    Backend last_backend() const noexcept { return last_backend_; }

    void query_ray(const Ray& ray, std::vector<Entity>& out);
    void query_frustum(const Frustum& fr, std::vector<Entity>& out);
    void query_sphere(math::Vec3 center, f32 radius, std::vector<Entity>& out);
    void query_aabb(const math::Aabb& box, std::vector<Entity>& out);
    void overlap_pairs(std::vector<std::pair<Entity, Entity>>& out);

   private:
    void ensure_sap();
    void ensure_grid();
    void map_indices(std::vector<Entity>& out) const;  // index scratch_ -> entities

    std::vector<Entity> entities_;
    std::vector<math::Aabb> aabbs_;
    Bvh bvh_;
    Sap sap_;
    UniformGrid grid_;
    std::vector<u32> scratch_;  ///< reusable index buffer for accelerator queries
    math::Aabb scene_bounds_ = aabb_empty();
    f32 scene_diagonal_ = 0.0f;  ///< cached length of scene_bounds_ extent (query routing)
    f32 cell_size_ = 1.0f;
    bool sap_dirty_ = true;
    bool grid_dirty_ = true;
    Backend last_backend_ = Backend::Bvh;
};

// ─── Brute-force baseline ────────────────────────────────────────────────────
// Linear reference implementations using the shared predicates. O(n) per query
// (O(n^2) for pairs); used by the unit tests to validate the accelerators.
namespace brute {

void query_ray(std::span<const math::Aabb> aabbs, const Ray& ray, std::vector<u32>& out);
void query_frustum(std::span<const math::Aabb> aabbs, const Frustum& fr, std::vector<u32>& out);
void query_sphere(std::span<const math::Aabb> aabbs, math::Vec3 center, f32 radius, std::vector<u32>& out);
void query_aabb(std::span<const math::Aabb> aabbs, const math::Aabb& box, std::vector<u32>& out);
void overlap_pairs(std::span<const math::Aabb> aabbs, std::vector<std::pair<u32, u32>>& out);

}  // namespace brute

}  // namespace psynder::scene
