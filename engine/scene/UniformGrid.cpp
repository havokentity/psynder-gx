// SPDX-License-Identifier: MIT
// Psynder — Lane 06 hashed uniform grid (see Spatial.h).

#include <algorithm>
#include <cmath>
#include <vector>

#include "Spatial.h"
#include "jobs/JobSystem.h"

namespace psynder::scene {

namespace {

constexpr usize kParallelGrain = 2048;

// Classic spatial-hash mix (Teschner et al.). i32 -> u32 wraps mod 2^32, which
// is well defined and exactly what we want for a hash.
u32 cell_hash(i32 x, i32 y, i32 z) noexcept {
    return static_cast<u32>(x) * 73856093u ^ static_cast<u32>(y) * 19349663u ^
           static_cast<u32>(z) * 83492791u;
}

usize next_pow2(usize x) noexcept {
    usize p = 16;
    while (p < x)
        p <<= 1u;
    return p;
}

}  // namespace

void UniformGrid::clear() noexcept {
    prims_ = {};
    table_.clear();
    used_.clear();
    entries_.clear();
    visit_.clear();
    epoch_ = 0;
    occupied_ = 0;
    mask_ = 0;
    cell_ = 1.0f;
    inv_cell_ = 1.0f;
}

void UniformGrid::build(std::span<const math::Aabb> aabbs, f32 cell_size) {
    prims_ = aabbs;
    cell_ = cell_size > 1.0e-4f ? cell_size : 1.0e-4f;  // guard against div-by-zero
    inv_cell_ = 1.0f / cell_;
    rebuild_internal();
}

void UniformGrid::rebuild() {
    rebuild_internal();
}

i32 UniformGrid::cell_coord(f32 v) const noexcept {
    return static_cast<i32>(std::floor(v * inv_cell_));
}

usize UniformGrid::find_slot(i32 cx, i32 cy, i32 cz) const noexcept {
    if (table_.empty())
        return table_.size();
    usize h = cell_hash(cx, cy, cz) & mask_;
    while (used_[h] != 0u) {
        const Cell& c = table_[h];
        if (c.x == cx && c.y == cy && c.z == cz)
            return h;
        h = (h + 1u) & mask_;
    }
    return table_.size();  // absent
}

void UniformGrid::rebuild_internal() {
    const u32 n = static_cast<u32>(prims_.size());
    table_.clear();
    used_.clear();
    entries_.clear();
    visit_.assign(n, 0u);
    epoch_ = 0;
    occupied_ = 0;
    mask_ = 0;
    if (n == 0u)
        return;

    // Per-proxy covering cell range, computed once in parallel and reused by
    // every pass below (each chunk writes disjoint slots).
    struct Range {
        i32 x0, x1, y0, y1, z0, z1;
    };
    std::vector<Range> ranges(n);
    jobs::JobSystem::Get().parallel_for(0u, n, kParallelGrain, [&](usize lo, usize hi) noexcept {
        for (usize i = lo; i < hi; ++i) {
            const math::Aabb& b = prims_[i];
            ranges[i] = Range{cell_coord(b.min.x),
                              cell_coord(b.max.x),
                              cell_coord(b.min.y),
                              cell_coord(b.max.y),
                              cell_coord(b.min.z),
                              cell_coord(b.max.z)};
        }
    });

    // Pass 1 — total (proxy, cell) insertions; bounds the table + entry array.
    usize total = 0;
    for (u32 i = 0; i < n; ++i) {
        const Range& r = ranges[i];
        total += static_cast<usize>(r.x1 - r.x0 + 1) * static_cast<usize>(r.y1 - r.y0 + 1) *
                 static_cast<usize>(r.z1 - r.z0 + 1);
    }

    const usize cap = next_pow2(total * 2u);  // load factor <= 0.5
    table_.assign(cap, Cell{});
    used_.assign(cap, 0u);
    mask_ = static_cast<u32>(cap - 1u);

    auto find_or_insert = [&](i32 cx, i32 cy, i32 cz) -> usize {
        usize h = cell_hash(cx, cy, cz) & mask_;
        while (used_[h] != 0u) {
            Cell& c = table_[h];
            if (c.x == cx && c.y == cy && c.z == cz)
                return h;
            h = (h + 1u) & mask_;
        }
        used_[h] = 1u;
        table_[h] = Cell{cx, cy, cz, 0u, 0u};
        ++occupied_;
        return h;
    };

    // Pass 2 — populate the cell table and per-cell counts.
    for (u32 i = 0; i < n; ++i) {
        const Range& r = ranges[i];
        for (i32 z = r.z0; z <= r.z1; ++z) {
            for (i32 y = r.y0; y <= r.y1; ++y) {
                for (i32 x = r.x0; x <= r.x1; ++x) {
                    ++table_[find_or_insert(x, y, z)].count;
                }
            }
        }
    }

    // Prefix-sum the counts into start offsets; reuse `count` as a fill cursor.
    u32 run = 0;
    for (usize s = 0; s < cap; ++s) {
        if (used_[s] != 0u) {
            table_[s].start = run;
            run += table_[s].count;
            table_[s].count = 0u;
        }
    }
    entries_.resize(run);  // run == total

    // Pass 3 — scatter proxy indices into their cell slices.
    for (u32 i = 0; i < n; ++i) {
        const Range& r = ranges[i];
        for (i32 z = r.z0; z <= r.z1; ++z) {
            for (i32 y = r.y0; y <= r.y1; ++y) {
                for (i32 x = r.x0; x <= r.x1; ++x) {
                    const usize slot = find_slot(x, y, z);
                    Cell& c = table_[slot];
                    entries_[c.start + c.count] = i;
                    ++c.count;
                }
            }
        }
    }
}

void UniformGrid::query_aabb(const math::Aabb& box, std::vector<u32>& out) {
    if (table_.empty())
        return;
    if (++epoch_ == 0u) {  // wrap: stale stamps could alias the new epoch
        std::fill(visit_.begin(), visit_.end(), 0u);
        epoch_ = 1;
    }
    const i32 x0 = cell_coord(box.min.x), x1 = cell_coord(box.max.x);
    const i32 y0 = cell_coord(box.min.y), y1 = cell_coord(box.max.y);
    const i32 z0 = cell_coord(box.min.z), z1 = cell_coord(box.max.z);

    for (i32 z = z0; z <= z1; ++z) {
        for (i32 y = y0; y <= y1; ++y) {
            for (i32 x = x0; x <= x1; ++x) {
                const usize slot = find_slot(x, y, z);
                if (slot >= table_.size())
                    continue;
                const Cell& c = table_[slot];
                for (u32 k = 0; k < c.count; ++k) {
                    const u32 p = entries_[c.start + k];
                    if (visit_[p] == epoch_)
                        continue;  // straddles multiple cells
                    visit_[p] = epoch_;
                    if (aabb_overlaps_aabb(prims_[p], box))
                        out.push_back(p);
                }
            }
        }
    }
}

void UniformGrid::query_sphere(math::Vec3 center, f32 radius, std::vector<u32>& out) {
    if (table_.empty())
        return;
    if (++epoch_ == 0u) {
        std::fill(visit_.begin(), visit_.end(), 0u);
        epoch_ = 1;
    }
    const math::Aabb sb{{center.x - radius, center.y - radius, center.z - radius},
                        {center.x + radius, center.y + radius, center.z + radius}};
    const i32 x0 = cell_coord(sb.min.x), x1 = cell_coord(sb.max.x);
    const i32 y0 = cell_coord(sb.min.y), y1 = cell_coord(sb.max.y);
    const i32 z0 = cell_coord(sb.min.z), z1 = cell_coord(sb.max.z);

    for (i32 z = z0; z <= z1; ++z) {
        for (i32 y = y0; y <= y1; ++y) {
            for (i32 x = x0; x <= x1; ++x) {
                const usize slot = find_slot(x, y, z);
                if (slot >= table_.size())
                    continue;
                const Cell& c = table_[slot];
                for (u32 k = 0; k < c.count; ++k) {
                    const u32 p = entries_[c.start + k];
                    if (visit_[p] == epoch_)
                        continue;
                    visit_[p] = epoch_;
                    if (aabb_intersects_sphere(prims_[p], center, radius))
                        out.push_back(p);
                }
            }
        }
    }
}

void UniformGrid::query_point(math::Vec3 p, std::vector<u32>& out) {
    if (table_.empty())
        return;
    const usize slot = find_slot(cell_coord(p.x), cell_coord(p.y), cell_coord(p.z));
    if (slot >= table_.size())
        return;
    const Cell& c = table_[slot];
    // A point lies in exactly one cell, so a proxy appears at most once here —
    // no dedup needed.
    for (u32 k = 0; k < c.count; ++k) {
        const u32 q = entries_[c.start + k];
        if (aabb_contains_point(prims_[q], p))
            out.push_back(q);
    }
}

}  // namespace psynder::scene
