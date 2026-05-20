// SPDX-License-Identifier: MIT
// Psynder — Lane 12 (world-bsp) Wave B unit tests: portal-based visibility
// culling (Portal.cpp / PortalGen.h) and lightmap atlas tile streaming
// (LightmapStream.h). TEST_CASE names are ASCII-only (see AGENTS.md).
//
// The portal-culling deliverable is exercised end-to-end: we cook a small
// .psybsp blob, load it through the VFS-backed loader (the "fixture"), build a
// portal graph from the BSP tree, and assert that portal culling is always a
// subset of the PVS — and that clipping the view frustum tightens the visible
// set below the full PVS.

#include <catch2/catch_test_macros.hpp>

#include <vector>  // Bsp.h uses std::vector without including <vector>.
#include "world/bsp/Bsp.h"
#include "world/bsp/BspFormat.h"
#include "world/bsp/Portal.h"
#include "world/bsp/PortalGen.h"
#include "world/bsp/LightmapStream.h"

#include "asset/Vfs.h"
#include "asset/VfsInternal.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>

namespace fs = std::filesystem;
using namespace psynder;
using namespace psynder::world::bsp;
using psynder::asset::Vfs;

namespace {

// ─── .psybsp fixture cooking ─────────────────────────────────────────────
// Lay out a header-prefixed blob exactly as BspFormat.h documents: a 96-byte
// header followed by node / leaf / face / vertex / index / pvs chunks. We omit
// vertices + indices (count 0) — portal culling and PVS need only the tree.
template <class T>
void append_pod(std::vector<u8>& out, const T& v) {
    const auto* p = reinterpret_cast<const u8*>(&v);
    out.insert(out.end(), p, p + sizeof(T));
}

std::vector<u8> cook_psybsp(const std::vector<BspFileNode>& nodes,
                            const std::vector<BspFileLeaf>& leaves,
                            const std::vector<BspFileFace>& faces,
                            const std::vector<u8>&          pvs,
                            u32 cluster_count, u32 pvs_row_bytes) {
    const u32 nodes_bytes  = static_cast<u32>(nodes.size()  * sizeof(BspFileNode));
    const u32 leaves_bytes = static_cast<u32>(leaves.size() * sizeof(BspFileLeaf));
    const u32 faces_bytes  = static_cast<u32>(faces.size()  * sizeof(BspFileFace));
    const u32 pvs_bytes    = static_cast<u32>(pvs.size());

    const u32 off_header = 0;
    const u32 off_nodes  = sizeof(BspFileHeader);            // 96
    const u32 off_leaves = off_nodes  + nodes_bytes;
    const u32 off_faces  = off_leaves + leaves_bytes;
    const u32 off_verts  = off_faces  + faces_bytes;         // 0-count
    const u32 off_idx    = off_verts;                        // 0-count
    const u32 off_pvs    = off_idx;
    const u32 total      = off_pvs + pvs_bytes;
    (void)off_header;

    BspFileHeader h{};
    h.magic         = kBspFileMagic;
    h.version       = kBspFileVersion;
    h.flags         = 0;
    h.total_bytes   = total;
    h.cluster_count = cluster_count;
    h.pvs_row_bytes = pvs_row_bytes;
    h.nodes    = BspFileChunk{ off_nodes,  static_cast<u32>(nodes.size())  };
    h.leaves   = BspFileChunk{ off_leaves, static_cast<u32>(leaves.size()) };
    h.faces    = BspFileChunk{ off_faces,  static_cast<u32>(faces.size())  };
    h.vertices = BspFileChunk{ off_verts,  0 };
    h.indices  = BspFileChunk{ off_idx,    0 };
    h.pvs      = BspFileChunk{ off_pvs,    pvs_bytes };

    std::vector<u8> blob;
    blob.reserve(total);
    append_pod(blob, h);
    for (const auto& n : nodes)  append_pod(blob, n);
    for (const auto& l : leaves) append_pod(blob, l);
    for (const auto& f : faces)  append_pod(blob, f);
    blob.insert(blob.end(), pvs.begin(), pvs.end());
    return blob;
}

// A 3-room corridor along X:  leaf0 (x<-1) | leaf1 (-1..1) | leaf2 (x>1).
//   node0: x = -1 (front = node1, back = leaf0)
//   node1: x = +1 (front = leaf2, back = leaf1)
// PVS: cluster0 -> {0,1}, cluster1 -> {0,1,2}, cluster2 -> {1,2}.
// Faces carry lightmap ids 10/11/12 so we can link culling to streaming.
std::vector<u8> cook_corridor() {
    std::vector<BspFileNode> nodes = {
        BspFileNode{ 1, 0, 0, -1.0f, /*front*/ 1,                 /*back*/ bsp_encode_leaf(0) },
        BspFileNode{ 1, 0, 0,  1.0f, /*front*/ bsp_encode_leaf(2), /*back*/ bsp_encode_leaf(1) },
    };
    std::vector<BspFileLeaf> leaves = {
        BspFileLeaf{ 0, 0, 1, -5, -2, -2, -1,  2,  2 },
        BspFileLeaf{ 1, 1, 1, -1, -2, -2,  1,  2,  2 },
        BspFileLeaf{ 2, 2, 1,  1, -2, -2,  5,  2,  2 },
    };
    std::vector<BspFileFace> faces = {
        BspFileFace{ 0, 4, 100, 10 },
        BspFileFace{ 0, 4, 101, 11 },
        BspFileFace{ 0, 4, 102, 12 },
    };
    // row0 = {0,1} = 0b011, row1 = {0,1,2} = 0b111, row2 = {1,2} = 0b110
    std::vector<u8> pvs = { 0x03, 0x07, 0x06 };
    return cook_psybsp(nodes, leaves, faces, pvs, /*clusters*/ 3, /*row_bytes*/ 1);
}

fs::path make_scratch_dir(const char* tag) {
    static int counter = 0;
    fs::path base = fs::temp_directory_path() / "psynder_world_bsp_waveb";
    fs::create_directories(base);
    fs::path d = base / (std::string(tag) + "_" + std::to_string(++counter));
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}

void write_binary(const fs::path& p, const std::vector<u8>& bytes) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
}

struct ResetGuard {
    ResetGuard()  { psynder::asset::internal::reset_for_tests(); }
    ~ResetGuard() { psynder::asset::internal::reset_for_tests(); }
};

// ─── visibility collectors ───────────────────────────────────────────────
struct PortalVis {
    const BspMap*    map = nullptr;
    std::set<i32>    clusters;
    std::set<u32>    lightmaps;
};

void portal_collect(const BspLeaf& leaf, const PortalFrustum&, void* user) {
    auto* v = static_cast<PortalVis*>(user);
    v->clusters.insert(leaf.cluster);
    for (u32 i = 0; i < leaf.face_count; ++i) {
        const u32 fi = leaf.first_face + i;
        if (v->map && fi < v->map->faces.size()) {
            v->lightmaps.insert(v->map->faces[fi].lightmap);
        }
    }
}

void pvs_collect(const BspLeaf& leaf, void* user) {
    static_cast<std::set<i32>*>(user)->insert(leaf.cluster);
}

PortalFrustum open_frustum() {
    PortalFrustum f{};
    f.plane_count = 0;  // no planes → everything inside
    return f;
}

PortalFrustum half_space(math::Vec3 n, f32 d) {
    PortalFrustum f{};
    f.normals[0]  = n;
    f.d[0]        = d;
    f.plane_count = 1;
    return f;
}

bool subset_of(const std::set<i32>& a, const std::set<i32>& b) {
    return std::includes(b.begin(), b.end(), a.begin(), a.end());
}

}  // namespace

TEST_CASE("world_bsp/waveb: portal culling is a subset of the PVS",
          "[world_bsp][portal]") {
    ResetGuard rg;
    auto dir = make_scratch_dir("corridor");
    write_binary(dir / "maps" / "corridor.psybsp", cook_corridor());
    REQUIRE(Vfs::Get().mount_directory(dir.string()));

    // Load the fixture through the real loader.
    BspMap map;
    REQUIRE(load("maps/corridor.psybsp", map));
    REQUIRE(map.leaves.size() == 3);
    REQUIRE(map.nodes.size() == 2);

    // Derive the portal graph from the tree. The corridor has exactly two
    // interior portals: leaf0<->leaf1 and leaf1<->leaf2.
    const BspPortalSet portals = build_portal_set_from_tree(map);
    REQUIRE(portals.portals.size() == 2);
    {
        std::set<std::set<i32>> pairs;
        for (const BspPortal& p : portals.portals) {
            pairs.insert(std::set<i32>{ p.front_leaf, p.back_leaf });
        }
        REQUIRE(pairs == std::set<std::set<i32>>{ {0, 1}, {1, 2} });
    }

    // Eye in the middle room (leaf1, cluster 1). Its PVS row is {0,1,2}.
    const math::Vec3 eye{ 0.0f, 0.0f, 0.0f };
    std::set<i32> pvs_set;
    walk_visible_leaves(map, eye, &pvs_collect, &pvs_set);
    REQUIRE(pvs_set == std::set<i32>{ 0, 1, 2 });

    SECTION("wide-open frustum: every portal-reachable leaf, still subset of PVS") {
        PortalVis vis{ &map, {}, {} };
        const PortalFrustum frustum = open_frustum();
        walk_portal_visible_leaves(map, portals, eye, frustum,
                                   &portal_collect, &vis);
        REQUIRE(subset_of(vis.clusters, pvs_set));
        REQUIRE(vis.clusters == std::set<i32>{ 0, 1, 2 });
    }

    SECTION("frustum facing +X clips the leaf behind the camera") {
        PortalVis vis{ &map, {}, {} };
        // Inside iff x >= 0: the leaf0 portal (at x=-1) falls outside it.
        const PortalFrustum frustum = half_space(math::Vec3{ 1, 0, 0 }, 0.0f);
        walk_portal_visible_leaves(map, portals, eye, frustum,
                                   &portal_collect, &vis);

        REQUIRE(subset_of(vis.clusters, pvs_set));      // never exceeds PVS
        REQUIRE(vis.clusters == std::set<i32>{ 1, 2 }); // strictly tighter
        REQUIRE(vis.clusters.count(0) == 0);            // leaf0 culled by frustum

        // The visible lightmap set follows the visible leaves: leaf1 (id 11)
        // and leaf2 (id 12) — leaf0's id 10 is culled.
        REQUIRE(vis.lightmaps == std::set<u32>{ 11, 12 });
    }
}

TEST_CASE("world_bsp/waveb: portal frustum clipping rejects leaves behind a wall",
          "[world_bsp][portal]") {
    // Two rooms split at x=0, with a hand-built portal — decoupled from the
    // tree portalizer so this exercises the traversal + frustum math directly.
    BspMap map;
    map.nodes = {
        BspNode{ math::Vec3{1, 0, 0}, 0.0f, bsp_encode_leaf(1), bsp_encode_leaf(0) },
    };
    BspLeaf l0{};
    l0.cluster = 0; l0.first_face = 0; l0.face_count = 0;
    l0.bounds.min = { -4, -2, -2 }; l0.bounds.max = { 0, 2, 2 };
    BspLeaf l1{};
    l1.cluster = 1; l1.first_face = 0; l1.face_count = 0;
    l1.bounds.min = { 0, -2, -2 }; l1.bounds.max = { 4, 2, 2 };
    map.leaves = { l0, l1 };
    // No PVS data → loader/walk fallback treats every cluster as visible, so
    // this isolates portal+frustum behaviour from PVS gating.

    BspPortalSet portals;
    portals.vertices = {
        { 0, -2, -2 }, { 0, 2, -2 }, { 0, 2, 2 }, { 0, -2, 2 },
    };
    BspPortal p{};
    p.front_leaf = 1;                      // +x side
    p.back_leaf  = 0;                      // -x side
    p.first_vertex = 0; p.vertex_count = 4;
    p.plane_normal = { -1, 0, 0 };         // front (leaf1) -> back (leaf0)
    p.plane_d      = 0.0f;
    portals.portals = { p };

    const math::Vec3 eye{ -1.0f, 0.0f, 0.0f };  // inside leaf0

    SECTION("frustum facing the portal sees the next room") {
        PortalVis vis{ &map, {}, {} };
        const PortalFrustum frustum = half_space(math::Vec3{ 1, 0, 0 }, -1.0f);
        walk_portal_visible_leaves(map, portals, eye, frustum,
                                   &portal_collect, &vis);
        REQUIRE(vis.clusters == std::set<i32>{ 0, 1 });
    }

    SECTION("frustum facing away keeps only the camera's room") {
        PortalVis vis{ &map, {}, {} };
        const PortalFrustum frustum = half_space(math::Vec3{ -1, 0, 0 }, 1.0f);
        walk_portal_visible_leaves(map, portals, eye, frustum,
                                   &portal_collect, &vis);
        REQUIRE(vis.clusters == std::set<i32>{ 0 });
    }
}

TEST_CASE("world_bsp/waveb: empty portal set degrades to PVS-only",
          "[world_bsp][portal]") {
    // Backwards-compat with the Wave A contract: no portals → PVS fallback.
    ResetGuard rg;
    auto dir = make_scratch_dir("corridor_pvs");
    write_binary(dir / "maps" / "corridor.psybsp", cook_corridor());
    REQUIRE(Vfs::Get().mount_directory(dir.string()));
    BspMap map;
    REQUIRE(load("maps/corridor.psybsp", map));

    const BspPortalSet empty;  // no portals
    REQUIRE(empty.portals.empty());

    PortalVis vis{ &map, {}, {} };
    walk_portal_visible_leaves(map, empty, math::Vec3{ -3, 0, 0 }, open_frustum(),
                               &portal_collect, &vis);
    // Eye in leaf0 (cluster 0); PVS row 0 = {0,1}.
    REQUIRE(vis.clusters == std::set<i32>{ 0, 1 });
}

TEST_CASE("world_bsp/waveb: lightmap streamer honours the atlas budget",
          "[world_bsp][lightmap]") {
    // Four tiles laid out along +X at 1 m spacing from the origin.
    std::vector<LightmapTile> tiles;
    for (u32 i = 0; i < 4; ++i) {
        LightmapTile t{};
        t.lightmap_id  = i;
        t.world_center = { 1.0f + static_cast<f32>(i), 0.0f, 0.0f };
        t.world_radius = 0.0f;
        t.width = t.height = 64;
        tiles.push_back(t);
    }

    LightmapStreamConfig cfg{};
    cfg.max_resident_tiles = 2;
    cfg.max_distance_m     = 0.0f;  // unbounded
    cfg.evict_hysteresis_m = 0.0f;

    LightmapStreamer streamer;
    streamer.configure(cfg, tiles);
    REQUIRE(streamer.tile_count() == 4);

    const std::vector<u32> all_visible = { 0, 1, 2, 3 };

    SECTION("only the budget's worth of nearest visible tiles are resident") {
        LightmapStreamDelta delta;
        streamer.update(math::Vec3{ 0, 0, 0 }, all_visible, delta);
        REQUIRE(streamer.resident_count() == 2);
        // Nearest two tiles are ids 0 and 1.
        REQUIRE(streamer.residency(0) == TileResidency::Resident);
        REQUIRE(streamer.residency(1) == TileResidency::Resident);
        REQUIRE(streamer.residency(2) == TileResidency::Evicted);
        REQUIRE(streamer.residency(3) == TileResidency::Evicted);
        REQUIRE(delta.paged_in == std::vector<u32>{ 0, 1 });
        REQUIRE(delta.paged_out.empty());
    }

    SECTION("invisible tiles are never resident") {
        LightmapStreamDelta delta;
        streamer.update(math::Vec3{ 0, 0, 0 }, std::vector<u32>{ 2, 3 }, delta);
        REQUIRE(streamer.resident_count() == 2);
        REQUIRE(streamer.residency(2) == TileResidency::Resident);
        REQUIRE(streamer.residency(3) == TileResidency::Resident);
        REQUIRE(streamer.residency(0) == TileResidency::Evicted);
    }

    SECTION("max_distance_m drops tiles that are too far") {
        LightmapStreamConfig far_cfg = cfg;
        far_cfg.max_distance_m = 2.5f;  // only tiles within 2.5 m (ids 0,1) qualify
        LightmapStreamer s2;
        s2.configure(far_cfg, tiles);
        LightmapStreamDelta delta;
        s2.update(math::Vec3{ 0, 0, 0 }, all_visible, delta);
        REQUIRE(s2.resident_count() == 2);
        REQUIRE(s2.residency(0) == TileResidency::Resident);
        REQUIRE(s2.residency(1) == TileResidency::Resident);
        REQUIRE(s2.residency(3) == TileResidency::Evicted);
    }
}

TEST_CASE("world_bsp/waveb: lightmap streamer hysteresis avoids thrash",
          "[world_bsp][lightmap]") {
    // Three tiles; budget 2. Frame 1 fixes residency, frame 2 nudges distances
    // so a non-resident tile becomes marginally closer than a resident one.
    // Three tiles at fixed positions along +X; we move the EYE to vary range.
    std::vector<LightmapTile> tiles(3);
    for (u32 i = 0; i < 3; ++i) {
        tiles[i].lightmap_id  = i;
        tiles[i].world_center = { static_cast<f32>(i), 0.0f, 0.0f };
        tiles[i].world_radius = 0.0f;
        tiles[i].width = tiles[i].height = 32;
    }
    const std::vector<u32> visible = { 0, 1, 2 };

    LightmapStreamConfig cfg{};
    cfg.max_resident_tiles = 2;
    cfg.evict_hysteresis_m = 0.0f;

    LightmapStreamConfig hcfg = cfg;
    hcfg.evict_hysteresis_m = 0.6f;

    // Without hysteresis: eye near tile0 first (resident {0,1}), then eye moves
    // so tile2 is marginally closer than tile1 -> tile1 evicted, tile2 paged in.
    LightmapStreamer plain;
    plain.configure(cfg, tiles);
    LightmapStreamDelta d;
    plain.update(math::Vec3{ 0, 0, 0 }, visible, d);          // dists 0,1,2 -> {0,1}
    REQUIRE(plain.residency(0) == TileResidency::Resident);
    REQUIRE(plain.residency(1) == TileResidency::Resident);
    plain.update(math::Vec3{ 2.05f, 0, 0 }, visible, d);      // dists 2.05,1.05,0.05
    // tile2 (0.05) and tile1 (1.05) are nearest -> tile0 evicted, tile2 in.
    REQUIRE(plain.residency(2) == TileResidency::Resident);
    REQUIRE(plain.residency(0) == TileResidency::Evicted);

    // With hysteresis: same motion, but a small move shouldn't churn residency.
    LightmapStreamer hyst;
    hyst.configure(hcfg, tiles);
    LightmapStreamDelta hd;
    hyst.update(math::Vec3{ 0, 0, 0 }, visible, hd);          // resident {0,1}
    REQUIRE(hyst.residency(0) == TileResidency::Resident);
    REQUIRE(hyst.residency(1) == TileResidency::Resident);
    hyst.update(math::Vec3{ 1.05f, 0, 0 }, visible, hd);      // dists 1.05,0.05,0.95
    // tile2 raw dist 0.95 vs resident tile0 raw 1.05 (+0.6 bonus = -0.45 score
    // beats tile2's -0.95) -> incumbents {0,1} hold; no thrash.
    REQUIRE(hyst.residency(0) == TileResidency::Resident);
    REQUIRE(hyst.residency(1) == TileResidency::Resident);
    REQUIRE(hyst.residency(2) == TileResidency::Evicted);
    REQUIRE(hd.paged_in.empty());
    REQUIRE(hd.paged_out.empty());
}
