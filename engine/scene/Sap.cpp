// SPDX-License-Identifier: MIT
// Psynder — Lane 06 sweep-and-prune broadphase (see Spatial.h).

#include <algorithm>
#include <utility>
#include <vector>

#include "Spatial.h"
#include "jobs/JobSystem.h"

namespace psynder::scene {

namespace {
constexpr usize kParallelGrain = 2048;
}  // namespace

void Sap::clear() noexcept {
    prims_ = {};
    sorted_.clear();
    axis_ = 0;
}

void Sap::build(std::span<const math::Aabb> aabbs) {
    prims_ = aabbs;
    rebuild();
}

void Sap::rebuild() {
    const u32 n = static_cast<u32>(prims_.size());
    sorted_.resize(n);
    if (n == 0u) {
        axis_ = 0;
        return;
    }

    // Proxy centres, in parallel (disjoint writes per index).
    std::vector<math::Vec3> centers(n);
    jobs::JobSystem::Get().parallel_for(0u, n, kParallelGrain, [&](usize lo, usize hi) noexcept {
        for (usize i = lo; i < hi; ++i) {
            centers[i] = aabb_center(prims_[i]);
        }
    });

    // Sweep on the axis of greatest centre variance: it produces the fewest
    // axis-overlapping candidate pairs, so the full 3-axis test runs least.
    math::Vec3 mean{0.0f, 0.0f, 0.0f};
    for (u32 i = 0; i < n; ++i) {
        mean.x += centers[i].x;
        mean.y += centers[i].y;
        mean.z += centers[i].z;
    }
    const f32 inv_n = 1.0f / static_cast<f32>(n);
    mean = {mean.x * inv_n, mean.y * inv_n, mean.z * inv_n};

    math::Vec3 var{0.0f, 0.0f, 0.0f};
    for (u32 i = 0; i < n; ++i) {
        const f32 dx = centers[i].x - mean.x;
        const f32 dy = centers[i].y - mean.y;
        const f32 dz = centers[i].z - mean.z;
        var.x += dx * dx;
        var.y += dy * dy;
        var.z += dz * dz;
    }
    axis_ = 0;
    f32 best = var.x;
    if (var.y > best) {
        best = var.y;
        axis_ = 1;
    }
    if (var.z > best) {
        axis_ = 2;
    }

    for (u32 i = 0; i < n; ++i)
        sorted_[i] = i;
    const u32 ax = axis_;
    std::sort(sorted_.begin(), sorted_.end(), [&](u32 a, u32 b) noexcept {
        return axis(prims_[a].min, ax) < axis(prims_[b].min, ax);
    });
}

void Sap::overlap_pairs(std::vector<std::pair<u32, u32>>& out) const {
    const u32 n = static_cast<u32>(prims_.size());
    if (n < 2u)
        return;
    const u32 ax = axis_;

    // The sweep is inherently sequential: it walks proxies in ascending
    // interval-start order, keeping the set of intervals still open on `ax`.
    std::vector<u32> active;
    active.reserve(64);
    for (u32 s = 0; s < n; ++s) {
        const u32 i = sorted_[s];
        const f32 lo_i = axis(prims_[i].min, ax);

        // Retire intervals that closed before i opened (swap-remove).
        for (usize k = 0; k < active.size();) {
            if (axis(prims_[active[k]].max, ax) < lo_i) {
                active[k] = active.back();
                active.pop_back();
            } else {
                ++k;
            }
        }

        // Everything still active overlaps i on `ax` -> confirm on all 3 axes.
        for (const u32 j : active) {
            if (aabb_overlaps_aabb(prims_[i], prims_[j])) {
                out.emplace_back(std::min(i, j), std::max(i, j));
            }
        }
        active.push_back(i);
    }
}

void Sap::query_aabb(const math::Aabb& box, std::vector<u32>& out) const {
    const u32 n = static_cast<u32>(prims_.size());
    if (n == 0u)
        return;
    const u32 ax = axis_;
    const f32 box_min = axis(box.min, ax);
    const f32 box_max = axis(box.max, ax);

    // Proxies whose interval starts after box_max cannot overlap; they form the
    // suffix of `sorted_`, so binary-search the prefix end and skip it.
    const auto end =
        std::upper_bound(sorted_.begin(), sorted_.end(), box_max, [&](f32 v, u32 idx) noexcept {
            return v < axis(prims_[idx].min, ax);
        });
    for (auto it = sorted_.begin(); it != end; ++it) {
        const u32 j = *it;
        if (axis(prims_[j].max, ax) >= box_min && aabb_overlaps_aabb(prims_[j], box)) {
            out.push_back(j);
        }
    }
}

}  // namespace psynder::scene
