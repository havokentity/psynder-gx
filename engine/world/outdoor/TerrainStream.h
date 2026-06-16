// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/world/outdoor/TerrainStream.h — headless terrain TILE streaming +
// CDLOD level-of-detail SELECTION for a Battlefield-light outdoor map.
//
// The CDLOD mesh (CdlodMesh_internal.h) and the heightfield (Heightmap_internal.h)
// answer "what does a chunk look like"; this module answers the orthogonal
// residency question: given a viewer position + a streaming radius, WHICH square
// terrain tiles should be loaded right now, at WHICH coarse CDLOD level each, and
// what is the DELTA from the previous frame (which tiles to LOAD, which to EVICT)
// so a streamer can act on it. It is the BF-light map's residency + LOD-pick
// logic — purely integer-tile based, no GPU, no allocation on the steady path.
//
// Model (all metric; 1 world unit = 1 metre):
//   * The terrain is a `tiles_x * tiles_z` grid of square tiles, each
//     `tile_size_m` on a side. Tile (x, z) covers world XZ
//     [x*tile_size_m, (x+1)*tile_size_m) x [z*tile_size_m, (z+1)*tile_size_m).
//     Only the X/Z plane matters for residency — height (Y) is ignored, the way
//     a 2D map-residency grid streams regardless of relief.
//   * RESIDENT set: every tile whose NEAREST point to the viewer (in XZ) is
//     within `stream_radius_m`. (A tile the viewer stands on has distance 0.)
//   * LOD per resident tile: `lod_for_distance(d)` where `d` is the distance from
//     the viewer to the tile CENTRE — near tiles = LOD 0 (high detail), far tiles
//     = higher LOD (coarser): lod = clamp(floor(d / lod_step_m), 0, max_lod).
//   * DELTA: diff this frame's resident set against the previous one to produce
//     `to_load` (newly resident) and `to_evict` (no longer resident). Applying
//     (old - to_evict + to_load) reproduces the new resident tile set exactly.
//
// Ordering: resident / to_load / to_evict are all in ASCENDING TileId order —
// z-major then x (row by row). This makes the residency, the LOD list, and the
// streaming deltas fully reproducible: identical viewer + identical config =>
// bitwise-identical resident / lod / load / evict. The math is pure algebra over
// the integer tile grid (the only sqrt is on the centre distance for the LOD
// pick), so there is no RNG and no ordering ambiguity. Cosmetic streaming, but
// deterministic so two machines that pick the same viewer residency-stream the
// same tiles in the same order.

#pragma once

#include "math/Math.h"   // math::Vec3
#include "core/Types.h"

#include <vector>

namespace psynder::world::outdoor {

// Streaming + LOD parameters for one terrain. All distances in metres.
struct TerrainStreamConfig {
    f32 tile_size_m   = 64.0f;   // side length of one square tile (metres)
    u32 tiles_x       = 0;       // tile count along world X
    u32 tiles_z       = 0;       // tile count along world Z
    f32 stream_radius_m = 256.0f;// residency radius (nearest-point, metres)
    f32 lod_step_m    = 64.0f;   // metres of centre-distance per LOD step
    u32 max_lod       = 4;       // coarsest LOD index (clamp ceiling)
};

// Integer grid coordinate of a tile. Stable ascending order is z-major then x,
// so a (z, x) lexicographic compare gives the row-by-row residency order.
struct TileId {
    u32 x = 0;
    u32 z = 0;

    friend constexpr bool operator==(TileId a, TileId b) noexcept {
        return a.x == b.x && a.z == b.z;
    }
    friend constexpr bool operator!=(TileId a, TileId b) noexcept {
        return !(a == b);
    }
    // Ascending: z-major then x. Total order over the tile grid.
    friend constexpr bool operator<(TileId a, TileId b) noexcept {
        return (a.z != b.z) ? (a.z < b.z) : (a.x < b.x);
    }
};

// A resident tile paired with the CDLOD level selected for it this frame.
struct TileLod {
    TileId tile{};
    u32    lod = 0;
};

// Headless residency + LOD selector. Drive it once per frame:
//   configure(cfg); update(viewer);
//   for (const TileLod& t : resident()) { ... }
//   for (const TileId& t : to_load())  stream_in(t);
//   for (const TileId& t : to_evict()) stream_out(t);
// `update` reuses its scratch buffers; nothing allocates on the steady path once
// the working vectors have grown to their high-water mark.
class TerrainStream {
public:
    // Set the tile grid + streaming params. Resets the previous-resident memory,
    // so the next `update` reports ALL resident tiles as `to_load` (cold start).
    void configure(const TerrainStreamConfig& cfg) noexcept;

    // Recompute residency + LOD at `viewer` (world space; only X/Z used) and the
    // load/evict delta from the prior frame. Idempotent: a second `update` at the
    // same viewer yields empty `to_load`/`to_evict` and an unchanged `resident`.
    void update(math::Vec3 viewer);

    // Resident tiles this frame, each with its LOD, ascending TileId order.
    const std::vector<TileLod>& resident() const noexcept { return resident_; }
    // Tiles that became resident since the last frame (ascending). Stream IN.
    const std::vector<TileId>&  to_load()  const noexcept { return to_load_; }
    // Tiles that stopped being resident since the last frame (ascending). Stream OUT.
    const std::vector<TileId>&  to_evict() const noexcept { return to_evict_; }

    // The CDLOD level for a tile-centre distance `d` (metres):
    //   clamp(floor(d / lod_step_m), 0, max_lod). Monotone non-decreasing in d,
    //   saturating at max_lod. `lod_step_m <= 0` collapses to LOD 0.
    u32 lod_for_distance(f32 d) const noexcept;

    const TerrainStreamConfig& config() const noexcept { return cfg_; }

private:
    TerrainStreamConfig cfg_{};

    std::vector<TileLod> resident_;   // current frame, ascending
    std::vector<TileId>  prev_;       // last frame's resident TileIds, ascending
    std::vector<TileId>  to_load_;
    std::vector<TileId>  to_evict_;

    // Scratch: this frame's resident TileIds (ascending), reused across updates.
    std::vector<TileId>  curr_;
};

}  // namespace psynder::world::outdoor
