// SPDX-License-Identifier: MIT
// Psynder — cluster PVS (potentially-visible-set) table. Lane 12 (world-bsp)
// owns. This is the precomputed-visibility core that feeds the ECS render
// extract draw list: given the view cluster, it answers "which clusters are
// visible?" in O(1) per query and culls a flat array of entity clusters down
// to the visible indices with no per-frame heap allocation in the query path.
//
// Storage is a dense row-major bitset: `cluster_count` rows, each row a packed
// bitset over `cluster_count` bits, laid out as u64 words. Row `from` bit `to`
// is set iff cluster `to` is potentially visible from cluster `from`. The
// layout is POD/trivially-copyable-friendly (a single std::vector<u64>) and
// SoA-stable: iteration order is the entity index order the caller passes in,
// so the visible-index list is deterministic.
//
// TODO(#46): GPU Hi-Z two-phase occlusion for the dynamic set; this PVS
// handles static indoor visibility.

#pragma once

#include "core/Types.h"

#include <cstddef>
#include <span>
#include <vector>

namespace psynder::world::bsp {

// Compact per-cluster visibility table. Resize once at load time; the hot path
// (`visible`, `cull`) performs no allocation.
class PvsTable {
public:
    PvsTable() = default;
    explicit PvsTable(u32 cluster_count) { reset(cluster_count); }

    // Re-shape to `cluster_count` clusters, all visibility bits cleared. This
    // is the only call that allocates.
    void reset(u32 cluster_count) {
        cluster_count_ = cluster_count;
        words_per_row_ = (static_cast<usize>(cluster_count) + 63u) / 64u;
        bits_.assign(static_cast<usize>(cluster_count) * words_per_row_, 0u);
    }

    [[nodiscard]] u32 cluster_count() const noexcept { return cluster_count_; }

    // Mark cluster `to` visible from cluster `from`. Out-of-range indices are
    // ignored (defensive; the cooker is the only writer in practice).
    void set_visible(u32 from, u32 to) noexcept {
        if (from >= cluster_count_ || to >= cluster_count_) return;
        const usize w = row_offset(from) + (to >> 6);
        bits_[w] |= (u64{1} << (to & 63u));
    }

    // True iff cluster `to` is potentially visible from cluster `from`. No
    // allocation, no branches beyond the bounds guard; safe to call per entity.
    [[nodiscard]] bool visible(u32 from, u32 to) const noexcept {
        if (from >= cluster_count_ || to >= cluster_count_) return false;
        const u64 word = bits_[row_offset(from) + (to >> 6)];
        return (word & (u64{1} << (to & 63u))) != 0u;
    }

    // Append, to `out_visible_indices`, the index of every entity whose cluster
    // is visible from `view_cluster`. `entity_clusters[i]` is the cluster id of
    // entity i; the emitted indices are in ascending entity order (stable /
    // deterministic). `out_visible_indices` is NOT cleared — the caller owns
    // its lifetime and can reserve once and reuse it across frames, so the
    // query itself does no heap allocation.
    void cull(u32 view_cluster,
              std::span<const u32> entity_clusters,
              std::vector<u32>& out_visible_indices) const {
        if (view_cluster >= cluster_count_) return;
        const usize base = row_offset(view_cluster);
        const u32 n = static_cast<u32>(entity_clusters.size());
        for (u32 i = 0; i < n; ++i) {
            const u32 c = entity_clusters[i];
            if (c >= cluster_count_) continue;
            const u64 word = bits_[base + (c >> 6)];
            if ((word & (u64{1} << (c & 63u))) != 0u) {
                out_visible_indices.push_back(i);
            }
        }
    }

private:
    [[nodiscard]] usize row_offset(u32 from) const noexcept {
        return static_cast<usize>(from) * words_per_row_;
    }

    u32                cluster_count_ = 0;
    usize              words_per_row_ = 0;
    std::vector<u64>   bits_;  // cluster_count_ rows of words_per_row_ u64s
};

}  // namespace psynder::world::bsp
