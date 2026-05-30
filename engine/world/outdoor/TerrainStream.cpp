// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/world/outdoor/TerrainStream.cpp — residency + CDLOD-LOD selection.
// See TerrainStream.h for the model. Pure integer-tile algebra: the residency
// test is a nearest-point-in-XZ distance compare, the LOD pick is a floor of the
// tile-centre distance over lod_step_m, and the delta is a linear merge of two
// ascending TileId lists. Deterministic + allocation-free on the steady path.

#include "world/outdoor/TerrainStream.h"

#include <cmath>     // std::sqrt, std::floor

namespace psynder::world::outdoor {

namespace {

// Squared XZ distance from the viewer to the NEAREST point of tile (tx, tz).
// The nearest point clamps the viewer into the tile's [lo, hi) extent per axis;
// a viewer inside the tile yields 0 on that axis. Squared to avoid a sqrt on the
// residency test (we compare against radius*radius).
f32 nearest_dist_sq_xz(const TerrainStreamConfig& cfg,
                       u32 tx, u32 tz,
                       f32 vx, f32 vz) noexcept {
    const f32 lo_x = static_cast<f32>(tx) * cfg.tile_size_m;
    const f32 hi_x = lo_x + cfg.tile_size_m;
    const f32 lo_z = static_cast<f32>(tz) * cfg.tile_size_m;
    const f32 hi_z = lo_z + cfg.tile_size_m;

    f32 dx = 0.0f;
    if (vx < lo_x)      dx = lo_x - vx;
    else if (vx > hi_x) dx = vx - hi_x;

    f32 dz = 0.0f;
    if (vz < lo_z)      dz = lo_z - vz;
    else if (vz > hi_z) dz = vz - hi_z;

    return dx * dx + dz * dz;
}

// XZ distance from the viewer to the CENTRE of tile (tx, tz) — drives the LOD.
f32 centre_dist_xz(const TerrainStreamConfig& cfg,
                   u32 tx, u32 tz,
                   f32 vx, f32 vz) noexcept {
    const f32 cx = (static_cast<f32>(tx) + 0.5f) * cfg.tile_size_m;
    const f32 cz = (static_cast<f32>(tz) + 0.5f) * cfg.tile_size_m;
    const f32 dx = cx - vx;
    const f32 dz = cz - vz;
    return std::sqrt(dx * dx + dz * dz);
}

}  // namespace

void TerrainStream::configure(const TerrainStreamConfig& cfg) noexcept {
    cfg_ = cfg;
    // Cold start: forget the previous residency so the next update streams in
    // everything. Keep the buffers' capacity for reuse.
    prev_.clear();
    resident_.clear();
    curr_.clear();
    to_load_.clear();
    to_evict_.clear();
}

u32 TerrainStream::lod_for_distance(f32 d) const noexcept {
    if (cfg_.lod_step_m <= 0.0f) return 0u;
    if (d <= 0.0f) return 0u;
    const f32 step = std::floor(d / cfg_.lod_step_m);
    // floor of a non-negative ratio is >= 0; clamp the ceiling at max_lod.
    if (step <= 0.0f) return 0u;
    const f32 capped = static_cast<f32>(cfg_.max_lod);
    if (step >= capped) return cfg_.max_lod;
    return static_cast<u32>(step);
}

void TerrainStream::update(math::Vec3 viewer) {
    const f32 vx = viewer.x;
    const f32 vz = viewer.z;
    const f32 r2 = cfg_.stream_radius_m * cfg_.stream_radius_m;

    // Build this frame's resident set (+ LODs) in ascending TileId order by
    // walking the grid z-major then x — the iteration order IS the sort order.
    resident_.clear();
    curr_.clear();
    for (u32 tz = 0; tz < cfg_.tiles_z; ++tz) {
        for (u32 tx = 0; tx < cfg_.tiles_x; ++tx) {
            const f32 dsq = nearest_dist_sq_xz(cfg_, tx, tz, vx, vz);
            if (dsq > r2) continue;  // nearest point outside the radius: not resident
            const TileId id{tx, tz};
            const f32 cd  = centre_dist_xz(cfg_, tx, tz, vx, vz);
            resident_.push_back(TileLod{id, lod_for_distance(cd)});
            curr_.push_back(id);
        }
    }

    // Delta vs the previous frame. Both `prev_` and `curr_` are ascending, so a
    // linear two-pointer merge yields load (in curr, not prev) + evict (in prev,
    // not curr) — each in ascending order, no extra sort.
    to_load_.clear();
    to_evict_.clear();
    usize i = 0;  // prev_
    usize j = 0;  // curr_
    while (i < prev_.size() && j < curr_.size()) {
        const TileId a = prev_[i];
        const TileId b = curr_[j];
        if (a == b) { ++i; ++j; }
        else if (a < b) { to_evict_.push_back(a); ++i; }   // gone this frame
        else            { to_load_.push_back(b);  ++j; }    // new this frame
    }
    for (; i < prev_.size(); ++i) to_evict_.push_back(prev_[i]);
    for (; j < curr_.size(); ++j) to_load_.push_back(curr_[j]);

    // Promote curr -> prev for the next frame's diff (swap keeps the capacity).
    prev_.swap(curr_);
}

}  // namespace psynder::world::outdoor
