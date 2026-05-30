// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/world_terrain_stream.cpp — terrain TILE streaming + CDLOD LOD
// selection (engine/world/outdoor/TerrainStream): given a viewer + a streaming
// radius, which square tiles are resident, at what coarse LOD each, and what is
// the load/evict delta when the viewer moves across the BF-light map. Pure
// integer-tile residency — headless, deterministic, no GPU.

#include "world/outdoor/TerrainStream.h"

#include "math/Math.h"
#include "core/Types.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <vector>

using namespace psynder;
using namespace psynder::world::outdoor;

namespace {

// Standard config for the residency tests: a 9x9 grid of 64 m tiles (576 m on a
// side). Stream radius 150 m, one LOD step every 50 m, capped at LOD 4.
TerrainStreamConfig make_cfg() {
    TerrainStreamConfig c{};
    c.tile_size_m     = 64.0f;
    c.tiles_x         = 9;
    c.tiles_z         = 9;
    c.stream_radius_m = 150.0f;
    c.lod_step_m      = 50.0f;
    c.max_lod         = 4;
    return c;
}

// World-space centre of the whole 9x9 grid = centre of tile (4,4).
math::Vec3 grid_centre(const TerrainStreamConfig& c) {
    const f32 half = 0.5f * static_cast<f32>(c.tiles_x) * c.tile_size_m;  // 288 m
    return math::Vec3{half, 0.0f, half};
}

bool resident_contains(const std::vector<TileLod>& r, TileId id) {
    for (const TileLod& t : r) if (t.tile == id) return true;
    return false;
}

u32 lod_of(const std::vector<TileLod>& r, TileId id) {
    for (const TileLod& t : r) if (t.tile == id) return t.lod;
    return 0xFFFFFFFFu;  // not present sentinel
}

std::vector<TileId> ids_of(const std::vector<TileLod>& r) {
    std::vector<TileId> out;
    out.reserve(r.size());
    for (const TileLod& t : r) out.push_back(t.tile);
    return out;
}

bool ascending(const std::vector<TileId>& v) {
    for (usize i = 1; i < v.size(); ++i) {
        if (!(v[i - 1] < v[i])) return false;  // strictly ascending, no dups
    }
    return true;
}

}  // namespace

TEST_CASE("terrain-stream: resident set is the tiles within the radius", "[terrain][streaming]") {
    TerrainStream s;
    const TerrainStreamConfig c = make_cfg();
    s.configure(c);
    s.update(grid_centre(c));  // viewer at centre of tile (4,4): world (288,0,288)

    const std::vector<TileLod>& r = s.resident();

    // The centre tile is always resident at distance 0.
    REQUIRE(resident_contains(r, TileId{4, 4}));

    // Nearest-point reach along the X axis from the viewer at the centre of tile
    // (4,4): the nearest point of a tile k columns away (k>=1) is (k-1)*64 + 32
    // wait — concretely the viewer sits 32 m inside tile (4,4), so tile (4+k) has
    // nearest point (k*64 - 32) m away. With radius 150 m: k=1 -> 32, k=2 -> 96
    // (both resident); k=3 -> 160 m > 150 (excluded). So the X band is columns
    // 2..6 (k up to 2 either side).
    REQUIRE(resident_contains(r, TileId{2, 4}));   // 2 left, nearest 96 m
    REQUIRE(resident_contains(r, TileId{6, 4}));   // 2 right
    REQUIRE_FALSE(resident_contains(r, TileId{1, 4}));  // 3 left, nearest 160 m: far, excluded
    REQUIRE_FALSE(resident_contains(r, TileId{7, 4}));  // 3 right
    REQUIRE_FALSE(resident_contains(r, TileId{0, 4}));  // 4 left
    REQUIRE_FALSE(resident_contains(r, TileId{8, 4}));  // 4 right

    // A far CORNER tile is excluded (its nearest point is well beyond 150 m).
    REQUIRE_FALSE(resident_contains(r, TileId{0, 0}));
    REQUIRE_FALSE(resident_contains(r, TileId{8, 8}));

    // Exact residency count: brute-force the same nearest-point test and compare.
    u32 expected = 0;
    for (u32 tz = 0; tz < c.tiles_z; ++tz) {
        for (u32 tx = 0; tx < c.tiles_x; ++tx) {
            const f32 lo_x = static_cast<f32>(tx) * c.tile_size_m;
            const f32 hi_x = lo_x + c.tile_size_m;
            const f32 lo_z = static_cast<f32>(tz) * c.tile_size_m;
            const f32 hi_z = lo_z + c.tile_size_m;
            const f32 vx = grid_centre(c).x;
            const f32 vz = grid_centre(c).z;
            f32 dx = 0.0f;
            if (vx < lo_x) dx = lo_x - vx; else if (vx > hi_x) dx = vx - hi_x;
            f32 dz = 0.0f;
            if (vz < lo_z) dz = lo_z - vz; else if (vz > hi_z) dz = vz - hi_z;
            if (dx * dx + dz * dz <= c.stream_radius_m * c.stream_radius_m) ++expected;
        }
    }
    REQUIRE(r.size() == expected);
    REQUIRE(expected > 0u);
}

TEST_CASE("terrain-stream: centre tile is LOD 0 and edge tiles are coarser", "[terrain][streaming]") {
    TerrainStream s;
    const TerrainStreamConfig c = make_cfg();
    s.configure(c);
    s.update(grid_centre(c));

    const std::vector<TileLod>& r = s.resident();

    // The tile the viewer stands on is centred 0 m away -> LOD 0 (high detail).
    REQUIRE(lod_of(r, TileId{4, 4}) == 0u);

    // A resident tile out near the radius edge is centred far away -> a higher
    // LOD. Tile (6,4) is centred at x = 6.5*64 = 416 m; viewer at 288 m -> 128 m
    // away -> floor(128/50) = 2.
    REQUIRE(resident_contains(r, TileId{6, 4}));
    REQUIRE(lod_of(r, TileId{6, 4}) == 2u);
    REQUIRE(lod_of(r, TileId{6, 4}) > lod_of(r, TileId{4, 4}));

    // The immediate neighbour (5,4) is centred 64 m away -> floor(64/50) = 1.
    REQUIRE(lod_of(r, TileId{5, 4}) == 1u);
}

TEST_CASE("terrain-stream: lod_for_distance ramps with distance and clamps at max_lod", "[terrain][streaming]") {
    TerrainStream s;
    TerrainStreamConfig c = make_cfg();
    c.lod_step_m = 50.0f;
    c.max_lod    = 4;
    s.configure(c);

    // floor(d / 50), clamped to 0..4.
    REQUIRE(s.lod_for_distance(0.0f)   == 0u);
    REQUIRE(s.lod_for_distance(10.0f)  == 0u);
    REQUIRE(s.lod_for_distance(49.0f)  == 0u);
    REQUIRE(s.lod_for_distance(50.0f)  == 1u);
    REQUIRE(s.lod_for_distance(120.0f) == 2u);
    REQUIRE(s.lod_for_distance(175.0f) == 3u);
    REQUIRE(s.lod_for_distance(210.0f) == 4u);

    // Monotone non-decreasing: each step is >= the previous.
    u32 last = 0;
    for (f32 d = 0.0f; d <= 600.0f; d += 7.0f) {
        const u32 l = s.lod_for_distance(d);
        REQUIRE(l >= last);
        last = l;
    }

    // Clamps hard at max_lod no matter how far.
    REQUIRE(s.lod_for_distance(1000.0f)   == 4u);
    REQUIRE(s.lod_for_distance(100000.0f) == 4u);

    // A degenerate lod_step collapses everything to LOD 0.
    TerrainStreamConfig flat = c;
    flat.lod_step_m = 0.0f;
    TerrainStream sf;
    sf.configure(flat);
    REQUIRE(sf.lod_for_distance(500.0f) == 0u);
}

TEST_CASE("terrain-stream: moving the viewer streams tiles in and out", "[terrain][streaming]") {
    TerrainStream s;
    const TerrainStreamConfig c = make_cfg();
    s.configure(c);

    // Frame 0 at the grid centre. Cold start: every resident tile loads, nothing
    // evicts.
    s.update(grid_centre(c));
    const std::vector<TileId> set0 = ids_of(s.resident());
    REQUIRE(s.to_load().size() == set0.size());
    REQUIRE(s.to_evict().empty());
    {
        std::vector<TileId> loaded = s.to_load();
        REQUIRE(loaded == set0);  // load order equals resident order
    }

    // Frame 1: shove the viewer +160 m along X (2.5 tiles). The residency window
    // slides; some tiles enter, some leave.
    math::Vec3 moved = grid_centre(c);
    moved.x += 160.0f;
    s.update(moved);
    const std::vector<TileId> set1 = ids_of(s.resident());

    REQUIRE_FALSE(s.to_load().empty());
    REQUIRE_FALSE(s.to_evict().empty());

    // to_load = set1 - set0 exactly; to_evict = set0 - set1 exactly.
    std::vector<TileId> exp_load;
    std::set_difference(set1.begin(), set1.end(), set0.begin(), set0.end(),
                        std::back_inserter(exp_load));
    std::vector<TileId> exp_evict;
    std::set_difference(set0.begin(), set0.end(), set1.begin(), set1.end(),
                        std::back_inserter(exp_evict));
    REQUIRE(s.to_load()  == exp_load);
    REQUIRE(s.to_evict() == exp_evict);

    // Applying (old - evict + load) reproduces the new resident set exactly.
    std::vector<TileId> rebuilt;
    std::set_difference(set0.begin(), set0.end(),
                        s.to_evict().begin(), s.to_evict().end(),
                        std::back_inserter(rebuilt));
    for (const TileId& id : s.to_load()) rebuilt.push_back(id);
    std::sort(rebuilt.begin(), rebuilt.end(),
              [](TileId a, TileId b) { return a < b; });
    REQUIRE(rebuilt == set1);

    // A stationary re-update at the same viewer yields no streaming work.
    s.update(moved);
    REQUIRE(s.to_load().empty());
    REQUIRE(s.to_evict().empty());
    REQUIRE(ids_of(s.resident()) == set1);
}

TEST_CASE("terrain-stream: resident load and evict lists are ascending", "[terrain][streaming]") {
    TerrainStream s;
    const TerrainStreamConfig c = make_cfg();
    s.configure(c);

    s.update(grid_centre(c));
    REQUIRE(ascending(ids_of(s.resident())));
    REQUIRE(ascending(s.to_load()));
    REQUIRE(s.to_evict().empty());

    math::Vec3 moved = grid_centre(c);
    moved.x -= 130.0f;
    moved.z += 95.0f;
    s.update(moved);
    REQUIRE(ascending(ids_of(s.resident())));
    REQUIRE(ascending(s.to_load()));
    REQUIRE(ascending(s.to_evict()));
}

TEST_CASE("terrain-stream: same viewer and config gives identical output", "[terrain][streaming][determinism]") {
    const TerrainStreamConfig c = make_cfg();
    const math::Vec3 viewer{217.5f, 12.0f, 333.25f};

    TerrainStream a;
    a.configure(c);
    a.update(viewer);

    TerrainStream b;
    b.configure(c);
    b.update(viewer);

    // Resident tiles + their LODs are bit-identical between the two instances.
    REQUIRE(a.resident().size() == b.resident().size());
    for (usize i = 0; i < a.resident().size(); ++i) {
        REQUIRE(a.resident()[i].tile == b.resident()[i].tile);
        REQUIRE(a.resident()[i].lod  == b.resident()[i].lod);
    }
    // Cold-start delta matches too (both stream in the same set in the same order).
    REQUIRE(a.to_load()  == b.to_load());
    REQUIRE(a.to_evict() == b.to_evict());

    // Re-running update on the SAME instance at the SAME viewer is idempotent.
    const std::vector<TileId> before = ids_of(a.resident());
    a.update(viewer);
    REQUIRE(ids_of(a.resident()) == before);
    REQUIRE(a.to_load().empty());
    REQUIRE(a.to_evict().empty());
}
