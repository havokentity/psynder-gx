// SPDX-License-Identifier: MIT
// Psynder — Lane 06 refittable BVH (see Spatial.h for the design overview).

#include <algorithm>
#include <vector>

#include "Spatial.h"
#include "jobs/JobSystem.h"

namespace psynder::scene {

namespace {

// parallel_for grain: ~2k elements per chunk lands a few-x over-decomposition
// on a typical core count without drowning in scheduling overhead.
constexpr usize kParallelGrain = 2048;

// A balanced (median-split) BVH has depth <= ceil(log2(N)) + 1, so an iterative
// DFS never needs more than depth+1 stack slots. 64 covers N up to 2^31.
constexpr u32 kMaxStack = 64;

// Iterative stack traversal shared by every query. `node_hit` decides whether a
// node's bounds are relevant (descend); `leaf_hit` is invoked per primitive in
// each surviving leaf.
template <class NodeHit, class LeafHit>
void traverse(const std::vector<Bvh::Node>& nodes,
              const std::vector<u32>& order,
              NodeHit&& node_hit,
              LeafHit&& leaf_hit) {
    if (nodes.empty())
        return;
    u32 stack[kMaxStack];
    u32 sp = 0;
    stack[sp++] = 0;  // root
    while (sp > 0) {
        const Bvh::Node& nd = nodes[stack[--sp]];
        if (!node_hit(nd.bounds))
            continue;
        if (nd.count != 0u) {
            const u32 base = nd.first;
            for (u32 k = 0; k < nd.count; ++k)
                leaf_hit(order[base + k]);
        } else {
            // Children were allocated contiguously: left = first, right = first+1.
            stack[sp++] = nd.first;
            stack[sp++] = nd.first + 1u;
        }
    }
}

}  // namespace

void Bvh::clear() noexcept {
    prims_ = {};
    nodes_.clear();
    order_.clear();
    nodes_by_depth_.clear();
    level_offsets_.clear();
    max_depth_ = 0;
}

u32 Bvh::alloc_node() {
    const u32 idx = static_cast<u32>(nodes_.size());
    nodes_.emplace_back();
    return idx;
}

void Bvh::build(std::span<const math::Aabb> aabbs, u32 leaf_size) {
    clear();
    prims_ = aabbs;
    leaf_size_ = std::max(1u, leaf_size);

    const u32 n = static_cast<u32>(aabbs.size());
    if (n == 0u)
        return;

    order_.resize(n);
    for (u32 i = 0; i < n; ++i)
        order_[i] = i;

    // Centroids drive the split axis + median; recompute them in parallel
    // (each chunk writes disjoint slots, so no synchronisation is needed).
    std::vector<math::Vec3> centroids(n);
    jobs::JobSystem::Get().parallel_for(0u, n, kParallelGrain, [&](usize lo, usize hi) noexcept {
        for (usize i = lo; i < hi; ++i) {
            centroids[i] = aabb_center(aabbs[i]);
        }
    });

    nodes_.reserve(static_cast<usize>(n) * 2u);
    const u32 root = alloc_node();
    build_into(root, 0u, n, 0u, centroids);

    build_levels();
    refit(aabbs);  // fill node bounds bottom-up (parallel per level)
}

void Bvh::build_into(u32 self, u32 first, u32 count, u32 depth, const std::vector<math::Vec3>& centroids) {
    nodes_[self].depth = depth;
    max_depth_ = std::max(max_depth_, depth);

    if (count <= leaf_size_) {
        nodes_[self].first = first;
        nodes_[self].count = count;  // leaf (count > 0)
        return;
    }

    // Longest centroid axis over [first, first+count).
    math::Vec3 cmin = centroids[order_[first]];
    math::Vec3 cmax = cmin;
    for (u32 k = first + 1u; k < first + count; ++k) {
        const math::Vec3 c = centroids[order_[k]];
        cmin = {std::min(cmin.x, c.x), std::min(cmin.y, c.y), std::min(cmin.z, c.z)};
        cmax = {std::max(cmax.x, c.x), std::max(cmax.y, c.y), std::max(cmax.z, c.z)};
    }
    const math::Vec3 ext{cmax.x - cmin.x, cmax.y - cmin.y, cmax.z - cmin.z};
    u32 ax = 0;
    f32 best = ext.x;
    if (ext.y > best) {
        best = ext.y;
        ax = 1;
    }
    if (ext.z > best) {
        ax = 2;
    }

    // Median split by primitive count -> balanced tree regardless of geometry.
    const u32 mid = count / 2u;
    u32* base = order_.data() + first;
    std::nth_element(base, base + mid, base + count, [&](u32 a, u32 b) noexcept {
        return axis(centroids[a], ax) < axis(centroids[b], ax);
    });

    const u32 left = alloc_node();
    const u32 right = alloc_node();  // right == left + 1 (contiguous allocation)
    nodes_[self].first = left;
    nodes_[self].count = 0u;  // internal

    build_into(left, first, mid, depth + 1u, centroids);
    build_into(right, first + mid, count - mid, depth + 1u, centroids);
}

void Bvh::build_levels() {
    const u32 node_n = static_cast<u32>(nodes_.size());
    nodes_by_depth_.resize(node_n);

    std::vector<u32> per_depth(static_cast<usize>(max_depth_) + 1u, 0u);
    for (const Node& nd : nodes_)
        ++per_depth[nd.depth];

    // Group order is deepest-first: group g holds the nodes at depth
    // (max_depth_ - g). Refit then processes children (deeper, lower g) before
    // their parents.
    level_offsets_.assign(static_cast<usize>(max_depth_) + 2u, 0u);
    u32 acc = 0;
    for (u32 g = 0; g <= max_depth_; ++g) {
        level_offsets_[g] = acc;
        acc += per_depth[max_depth_ - g];
    }
    level_offsets_[max_depth_ + 1u] = acc;

    std::vector<u32> cursor(level_offsets_.begin(), level_offsets_.end() - 1);
    for (u32 ni = 0; ni < node_n; ++ni) {
        const u32 g = max_depth_ - nodes_[ni].depth;
        nodes_by_depth_[cursor[g]++] = ni;
    }
}

void Bvh::refit(std::span<const math::Aabb> aabbs) {
    if (nodes_.empty())
        return;
    prims_ = aabbs;  // re-borrow; length must equal the built primitive count

    jobs::JobSystem& js = jobs::JobSystem::Get();
    const u32 groups = static_cast<u32>(level_offsets_.size()) - 1u;
    for (u32 g = 0; g < groups; ++g) {
        const u32 b = level_offsets_[g];
        const u32 e = level_offsets_[g + 1u];
        // Within a depth group, every node writes only its own bounds and reads
        // only deeper (already-finished) children -> race-free parallel chunks.
        js.parallel_for(b, e, kParallelGrain, [&](usize lo, usize hi) noexcept {
            for (usize t = lo; t < hi; ++t) {
                const u32 ni = nodes_by_depth_[t];
                Node& nd = nodes_[ni];
                if (nd.count != 0u) {
                    math::Aabb bb = prims_[order_[nd.first]];
                    for (u32 k = 1u; k < nd.count; ++k) {
                        bb = aabb_merge(bb, prims_[order_[nd.first + k]]);
                    }
                    nd.bounds = bb;
                } else {
                    nd.bounds = aabb_merge(nodes_[nd.first].bounds, nodes_[nd.first + 1u].bounds);
                }
            }
        });
    }
}

math::Aabb Bvh::bounds() const noexcept {
    return nodes_.empty() ? aabb_empty() : nodes_[0].bounds;
}

void Bvh::query_ray(const Ray& ray, std::vector<u32>& out) const {
    const math::Vec3 inv = ray_inv_dir(ray);
    traverse(
        nodes_,
        order_,
        [&](const math::Aabb& nb) noexcept {
            return aabb_intersects_ray(nb, ray.origin, inv, ray.tmax);
        },
        [&](u32 p) {
            if (aabb_intersects_ray(prims_[p], ray.origin, inv, ray.tmax))
                out.push_back(p);
        });
}

void Bvh::query_frustum(const Frustum& fr, std::vector<u32>& out) const {
    traverse(
        nodes_,
        order_,
        [&](const math::Aabb& nb) noexcept { return aabb_in_frustum(nb, fr); },
        [&](u32 p) {
            if (aabb_in_frustum(prims_[p], fr))
                out.push_back(p);
        });
}

void Bvh::query_aabb(const math::Aabb& box, std::vector<u32>& out) const {
    traverse(
        nodes_,
        order_,
        [&](const math::Aabb& nb) noexcept { return aabb_overlaps_aabb(nb, box); },
        [&](u32 p) {
            if (aabb_overlaps_aabb(prims_[p], box))
                out.push_back(p);
        });
}

void Bvh::query_sphere(math::Vec3 center, f32 radius, std::vector<u32>& out) const {
    traverse(
        nodes_,
        order_,
        [&](const math::Aabb& nb) noexcept { return aabb_intersects_sphere(nb, center, radius); },
        [&](u32 p) {
            if (aabb_intersects_sphere(prims_[p], center, radius))
                out.push_back(p);
        });
}

}  // namespace psynder::scene
