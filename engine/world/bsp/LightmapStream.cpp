// SPDX-License-Identifier: MIT
// Psynder — lightmap atlas tile streaming. Lane 12 owns. See LightmapStream.h.

#include "LightmapStream.h"

#include "jobs/JobSystem.h"
#include "math/Math.h"

#include <algorithm>
#include <limits>

namespace psynder::world::bsp {

namespace {
constexpr u32 kNoTile = static_cast<u32>(-1);
const f32     kNegInf = -std::numeric_limits<f32>::infinity();
}  // namespace

void LightmapStreamer::configure(const LightmapStreamConfig&   cfg,
                                 std::span<const LightmapTile> tiles) {
    cfg_ = cfg;
    tiles_.assign(tiles.begin(), tiles.end());
    score_.assign(tiles_.size(), kNegInf);
    resident_.assign(tiles_.size(), static_cast<u8>(TileResidency::Evicted));
    resident_count_ = 0;
    // Size the per-frame scratch once so update() is allocation-free.
    visible_.assign(tiles_.size(), 0);
    want_.assign(tiles_.size(), 0);
    candidates_.clear();
    candidates_.reserve(tiles_.size());

    u32 max_id = 0;
    for (const LightmapTile& t : tiles_) {
        max_id = std::max(max_id, t.lightmap_id);
    }
    id_to_index_.assign(tiles_.empty() ? 0 : static_cast<usize>(max_id) + 1, kNoTile);
    for (u32 i = 0; i < tiles_.size(); ++i) {
        id_to_index_[tiles_[i].lightmap_id] = i;
    }
}

void LightmapStreamer::update(math::Vec3            eye,
                              std::span<const u32>  visible_lightmap_ids,
                              LightmapStreamDelta&  delta) {
    delta.paged_in.clear();
    delta.paged_out.clear();
    const usize n = tiles_.size();
    if (n == 0) {
        return;
    }

    // Mark the tiles referenced by visible faces this frame (reuse scratch).
    std::fill(visible_.begin(), visible_.end(), static_cast<u8>(0));
    for (const u32 id : visible_lightmap_ids) {
        if (id < id_to_index_.size()) {
            const u32 idx = id_to_index_[id];
            if (idx != kNoTile) {
                visible_[idx] = 1;
            }
        }
    }

    // Per-tile priority: visible + closer ranks higher. Embarrassingly parallel
    // — each lane writes only its own score_ slot. JobSystem runs this inline
    // until lane 04's worker pool lands, then it fans out for free.
    const f32 max_d = cfg_.max_distance_m;
    const f32 hys   = cfg_.evict_hysteresis_m;
    jobs::JobSystem::Get().parallel_for(0, n, 256, [&](usize lo, usize hi) {
        for (usize i = lo; i < hi; ++i) {
            if (!visible_[i]) {
                score_[i] = kNegInf;
                continue;
            }
            const math::Vec3 to = math::sub(tiles_[i].world_center, eye);
            f32 dist = math::length(to) - tiles_[i].world_radius;
            if (dist < 0.0f) {
                dist = 0.0f;
            }
            if (max_d > 0.0f && dist > max_d) {
                score_[i] = kNegInf;
                continue;
            }
            f32 s = -dist;  // closer ⇒ higher priority
            if (resident_[i] == static_cast<u8>(TileResidency::Resident)) {
                s += hys;   // incumbency headroom (anti-thrash)
            }
            score_[i] = s;
        }
    });

    // Desired resident set = the top `max_resident_tiles` candidates by score.
    candidates_.clear();
    for (u32 i = 0; i < n; ++i) {
        if (score_[i] > kNegInf) {
            candidates_.push_back(i);
        }
    }
    const auto by_priority = [&](u32 a, u32 b) {
        if (score_[a] != score_[b]) {
            return score_[a] > score_[b];
        }
        return tiles_[a].lightmap_id < tiles_[b].lightmap_id;  // stable tiebreak
    };
    const u32 budget = cfg_.max_resident_tiles;
    if (budget < candidates_.size()) {
        std::nth_element(candidates_.begin(),
                         candidates_.begin() + budget,
                         candidates_.end(), by_priority);
        candidates_.resize(budget);
    }

    std::fill(want_.begin(), want_.end(), static_cast<u8>(0));
    for (const u32 i : candidates_) {
        want_[i] = 1;
    }

    // Diff against current residency, then commit.
    u32 rc = 0;
    for (u32 i = 0; i < n; ++i) {
        const bool was = resident_[i] == static_cast<u8>(TileResidency::Resident);
        const bool now = want_[i] != 0;
        if (now && !was) {
            delta.paged_in.push_back(tiles_[i].lightmap_id);
        } else if (!now && was) {
            delta.paged_out.push_back(tiles_[i].lightmap_id);
        }
        resident_[i] = static_cast<u8>(now ? TileResidency::Resident
                                           : TileResidency::Evicted);
        if (now) {
            ++rc;
        }
    }
    resident_count_ = rc;
    std::sort(delta.paged_in.begin(), delta.paged_in.end());
    std::sort(delta.paged_out.begin(), delta.paged_out.end());
}

TileResidency LightmapStreamer::residency(u32 lightmap_id) const {
    if (lightmap_id < id_to_index_.size()) {
        const u32 idx = id_to_index_[lightmap_id];
        if (idx != kNoTile) {
            return static_cast<TileResidency>(resident_[idx]);
        }
    }
    return TileResidency::Evicted;
}

}  // namespace psynder::world::bsp
