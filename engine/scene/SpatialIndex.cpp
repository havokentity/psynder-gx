// SPDX-License-Identifier: MIT
// Psynder — Lane 06 spatial query router + brute-force baseline (see Spatial.h).

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

#include "Spatial.h"

namespace psynder::scene {

namespace {

// A query is routed to the grid when its extent is at most this fraction of the
// scene diagonal; larger queries touch too many cells, so the BVH wins.
constexpr f32 kCompactQueryFraction = 0.25f;

f32 scene_diagonal(const math::Aabb& b) noexcept {
    const math::Vec3 e = aabb_extent(b);
    return std::sqrt(e.x * e.x + e.y * e.y + e.z * e.z);
}

}  // namespace

// ─── SpatialIndex (router) ───────────────────────────────────────────────────

void SpatialIndex::clear() noexcept {
    entities_.clear();
    aabbs_.clear();
    bvh_.clear();
    sap_.clear();
    grid_.clear();
    scratch_.clear();
    scene_bounds_ = aabb_empty();
    cell_size_ = 1.0f;
    sap_dirty_ = true;
    grid_dirty_ = true;
    last_backend_ = Backend::Bvh;
}

void SpatialIndex::rebuild(std::span<const Entity> entities,
                           std::span<const math::Aabb> aabbs,
                           Params params) {
    entities_.assign(entities.begin(), entities.end());
    aabbs_.assign(aabbs.begin(), aabbs.end());

    // Cell size: caller override, else the mean proxy extent clamped to >= 1 m
    // so a typical proxy spans only a handful of cells.
    if (params.cell_size > 0.0f) {
        cell_size_ = params.cell_size;
    } else if (!aabbs_.empty()) {
        f64 sum = 0.0;
        for (const math::Aabb& b : aabbs_) {
            const math::Vec3 e = aabb_extent(b);
            sum += static_cast<f64>(std::max({e.x, e.y, e.z}));
        }
        const f32 mean = static_cast<f32>(sum / static_cast<f64>(aabbs_.size()));
        cell_size_ = std::max(mean, 1.0f);
    } else {
        cell_size_ = 1.0f;
    }

    bvh_.build(std::span<const math::Aabb>{aabbs_}, params.bvh_leaf_size);
    scene_bounds_ = bvh_.bounds();
    sap_dirty_ = true;
    grid_dirty_ = true;
}

void SpatialIndex::refit(std::span<const math::Aabb> aabbs) {
    // Precondition: same proxy count and order as the last rebuild() — only the
    // bounds have moved. The BVH refits cheaply in place; the SAP/grid keep no
    // incremental state here, so they are simply marked for lazy rebuild.
    aabbs_.assign(aabbs.begin(), aabbs.end());
    bvh_.refit(std::span<const math::Aabb>{aabbs_});
    scene_bounds_ = bvh_.bounds();
    sap_dirty_ = true;
    grid_dirty_ = true;
}

void SpatialIndex::ensure_sap() {
    if (sap_dirty_) {
        sap_.build(std::span<const math::Aabb>{aabbs_});
        sap_dirty_ = false;
    }
}

void SpatialIndex::ensure_grid() {
    if (grid_dirty_) {
        grid_.build(std::span<const math::Aabb>{aabbs_}, cell_size_);
        grid_dirty_ = false;
    }
}

void SpatialIndex::map_indices(std::vector<Entity>& out) const {
    out.clear();
    out.reserve(scratch_.size());
    for (const u32 idx : scratch_)
        out.push_back(entities_[idx]);
}

void SpatialIndex::query_ray(const Ray& ray, std::vector<Entity>& out) {
    out.clear();
    if (empty())
        return;
    last_backend_ = Backend::Bvh;
    scratch_.clear();
    bvh_.query_ray(ray, scratch_);
    map_indices(out);
}

void SpatialIndex::query_frustum(const Frustum& fr, std::vector<Entity>& out) {
    out.clear();
    if (empty())
        return;
    last_backend_ = Backend::Bvh;
    scratch_.clear();
    bvh_.query_frustum(fr, scratch_);
    map_indices(out);
}

void SpatialIndex::query_sphere(math::Vec3 center, f32 radius, std::vector<Entity>& out) {
    out.clear();
    if (empty())
        return;
    scratch_.clear();
    const bool compact = 2.0f * radius <= kCompactQueryFraction * scene_diagonal(scene_bounds_);
    if (compact) {
        ensure_grid();
        last_backend_ = Backend::Grid;
        grid_.query_sphere(center, radius, scratch_);
    } else {
        last_backend_ = Backend::Bvh;
        bvh_.query_sphere(center, radius, scratch_);
    }
    map_indices(out);
}

void SpatialIndex::query_aabb(const math::Aabb& box, std::vector<Entity>& out) {
    out.clear();
    if (empty())
        return;
    scratch_.clear();
    const math::Vec3 e = aabb_extent(box);
    const f32 max_ext = std::max({e.x, e.y, e.z});
    const bool compact = max_ext <= kCompactQueryFraction * scene_diagonal(scene_bounds_);
    if (compact) {
        ensure_grid();
        last_backend_ = Backend::Grid;
        grid_.query_aabb(box, scratch_);
    } else {
        last_backend_ = Backend::Bvh;
        bvh_.query_aabb(box, scratch_);
    }
    map_indices(out);
}

void SpatialIndex::overlap_pairs(std::vector<std::pair<Entity, Entity>>& out) {
    out.clear();
    if (empty())
        return;
    ensure_sap();
    last_backend_ = Backend::Sap;

    std::vector<std::pair<u32, u32>> idx;
    sap_.overlap_pairs(idx);
    out.reserve(idx.size());
    for (const auto& pr : idx) {
        out.emplace_back(entities_[pr.first], entities_[pr.second]);
    }
}

// ─── Brute-force baseline ────────────────────────────────────────────────────

namespace brute {

void query_ray(std::span<const math::Aabb> aabbs, const Ray& ray, std::vector<u32>& out) {
    const math::Vec3 inv = ray_inv_dir(ray);
    for (u32 i = 0; i < aabbs.size(); ++i) {
        if (aabb_intersects_ray(aabbs[i], ray.origin, inv, ray.tmax))
            out.push_back(i);
    }
}

void query_frustum(std::span<const math::Aabb> aabbs, const Frustum& fr, std::vector<u32>& out) {
    for (u32 i = 0; i < aabbs.size(); ++i) {
        if (aabb_in_frustum(aabbs[i], fr))
            out.push_back(i);
    }
}

void query_sphere(std::span<const math::Aabb> aabbs, math::Vec3 center, f32 radius, std::vector<u32>& out) {
    for (u32 i = 0; i < aabbs.size(); ++i) {
        if (aabb_intersects_sphere(aabbs[i], center, radius))
            out.push_back(i);
    }
}

void query_aabb(std::span<const math::Aabb> aabbs, const math::Aabb& box, std::vector<u32>& out) {
    for (u32 i = 0; i < aabbs.size(); ++i) {
        if (aabb_overlaps_aabb(aabbs[i], box))
            out.push_back(i);
    }
}

void overlap_pairs(std::span<const math::Aabb> aabbs, std::vector<std::pair<u32, u32>>& out) {
    const u32 n = static_cast<u32>(aabbs.size());
    for (u32 i = 0; i < n; ++i) {
        for (u32 j = i + 1u; j < n; ++j) {
            if (aabb_overlaps_aabb(aabbs[i], aabbs[j]))
                out.emplace_back(i, j);
        }
    }
}

}  // namespace brute

}  // namespace psynder::scene
