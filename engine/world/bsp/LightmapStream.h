// SPDX-License-Identifier: MIT
// Psynder — lightmap atlas tile streaming. Lane 12 (world-bsp) owns.
//
// lm_bake (lane 24) bakes static indoor lighting into a set of lightmap atlas
// tiles, one per BSP surface group (BspFace::lightmap is the tile id). The
// whole baked set lives in CPU memory at level load (DESIGN §9.1: maps load as
// a unit), but the GPU-resident atlas is a *bounded* page pool — far smaller
// than the full baked set on a large indoor map. This streamer decides which
// tiles should occupy that pool each frame from (a) the visible set produced by
// portal / PVS culling and (b) distance from the camera, and emits the page-in
// / page-out deltas the renderer (lane 09) turns into atlas uploads / frees.
//
// It is a pure CPU residency policy — no GPU types, and update() does no heap
// allocation: the scratch buffers are sized once in configure(), and the
// caller-owned delta vectors reuse their capacity across frames. That keeps it
// unit-testable in isolation and lets the per-tile scoring fan out over
// psynder::jobs::JobSystem.

#pragma once

#include <vector>  // public-header std::vector convention; see Bsp.cpp.
#include "Bsp.h"
#include "math/Math.h"

#include <span>

namespace psynder::world::bsp {

// One streamable lightmap atlas tile. Positions are real metric units (metres).
struct LightmapTile {
    u32        lightmap_id  = 0;          // == BspFace::lightmap
    math::Vec3 world_center{0, 0, 0};     // centroid of surfaces using this tile
    f32        world_radius = 0.0f;       // bounding radius of those surfaces
    u16        width        = 0;          // texel dimensions (atlas budgeting)
    u16        height       = 0;
};

// Streaming budget / policy.
struct LightmapStreamConfig {
    u32 max_resident_tiles = 0;     // GPU atlas page budget (hard cap)
    f32 max_distance_m     = 0.0f;  // tiles farther than this are never resident
                                    // (<= 0 → unbounded)
    f32 evict_hysteresis_m = 0.0f;  // resident tiles get this much priority
                                    // headroom so a marginally-closer newcomer
                                    // doesn't evict them (anti-thrash band)
};

enum class TileResidency : u8 {
    Evicted  = 0,
    Resident = 1,
};

// Tiles whose residency changed this frame. lightmap_ids are sorted ascending
// for deterministic output.
struct LightmapStreamDelta {
    std::vector<u32> paged_in;   // newly resident — renderer uploads these
    std::vector<u32> paged_out;  // newly evicted  — renderer frees these
};

// Distance-and-visibility-driven residency manager for the lightmap atlas.
class LightmapStreamer {
public:
    // Install the budget and the full baked tile set (copied in). Resets all
    // tiles to Evicted.
    void configure(const LightmapStreamConfig& cfg,
                   std::span<const LightmapTile> tiles);

    // Recompute residency for one frame. `visible_lightmap_ids` is the set of
    // lightmap ids reachable this frame (gathered from the faces of the leaves
    // that portal / PVS culling reported visible). Fills `delta` with the
    // page-in / page-out lists and updates the internal residency table.
    void update(math::Vec3 eye,
                std::span<const u32> visible_lightmap_ids,
                LightmapStreamDelta& delta);

    TileResidency residency(u32 lightmap_id) const;
    u32           resident_count() const noexcept { return resident_count_; }
    usize         tile_count() const noexcept { return tiles_.size(); }

private:
    LightmapStreamConfig      cfg_{};
    std::vector<LightmapTile> tiles_;
    std::vector<f32>          score_;        // per-tile priority (parallel scratch)
    std::vector<u8>           resident_;     // per-tile TileResidency
    std::vector<u32>          id_to_index_;  // dense lightmap_id -> tile index
    u32                       resident_count_ = 0;
    // Per-frame scratch, sized once in configure() so update() never allocates.
    std::vector<u8>           visible_;      // per-tile visible-this-frame flags
    std::vector<u32>          candidates_;   // visible tile indices
    std::vector<u8>           want_;         // per-tile desired-resident flags
};

}  // namespace psynder::world::bsp
