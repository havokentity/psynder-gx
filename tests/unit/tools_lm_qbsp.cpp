// SPDX-License-Identifier: MIT
// Psynder-GX - Lane 24 lm_qbsp test suite.
//
// Exercises the .map parser (MapSource.h) and the BSP compiler (LmQbsp.h):
// parse correctness, brush-plane orientation, SolidBSP solid/empty
// classification, exterior cull, connectivity PVS, the .psybsp wire format,
// determinism, and round-trip through the engine loader's validation rules.
//
// The tool logic is header-only, so this suite includes it through a
// relative path and runs in the default (PSYNDER_GX_BUILD_TOOLS=OFF) test
// build - no TOOLS-gated static library needed.

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "../../tools/lm_qbsp/LmQbsp.h"
#include "../../tools/lm_qbsp/MapSource.h"
#include "world/bsp/BspFormat.h"

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;
namespace bspfmt = psynder::world::bsp;

namespace {

// ── .map text builders ───────────────────────────────────────────────────

// Emit a standard-format axis-aligned box brush spanning [lo, hi].
std::string box_brush(double lx, double ly, double lz, double hx, double hy, double hz, const char* tex) {
    std::ostringstream o;
    auto face =
        [&](double ax, double ay, double az, double bx, double by, double bz, double cx, double cy, double cz) {
            o << "( " << ax << " " << ay << " " << az << " ) "
              << "( " << bx << " " << by << " " << bz << " ) "
              << "( " << cx << " " << cy << " " << cz << " ) " << tex << " 0 0 0 1 1\n";
        };
    o << "{\n";
    face(hx, ly, lz, hx, hy, lz, hx, hy, hz);  // +X
    face(lx, ly, lz, lx, hy, hz, lx, hy, lz);  // -X
    face(lx, hy, lz, hx, hy, lz, hx, hy, hz);  // +Y
    face(lx, ly, lz, hx, ly, hz, hx, ly, lz);  // -Y
    face(lx, ly, hz, hx, hy, hz, hx, ly, hz);  // +Z
    face(lx, ly, lz, hx, ly, lz, hx, hy, lz);  // -Z
    o << "}\n";
    return o.str();
}

// A hollow room: 6 wall slabs of thickness `t` lining the box [-hx,hx] etc.
std::string hollow_room(double hx, double hy, double hz, double t) {
    std::ostringstream o;
    o << "{\n\"classname\" \"worldspawn\"\n";
    o << box_brush(-hx, -hy, -hz, hx, hy, -hz + t, "FLOOR");
    o << box_brush(-hx, -hy, hz - t, hx, hy, hz, "CEIL");
    o << box_brush(-hx, -hy, -hz, -hx + t, hy, hz, "WEST");
    o << box_brush(hx - t, -hy, -hz, hx, hy, hz, "EAST");
    o << box_brush(-hx, -hy, -hz, hx, -hy + t, hz, "SOUTH");
    o << box_brush(-hx, hy - t, -hz, hx, hy, hz, "NORTH");
    o << "}\n";
    return o.str();
}

// ── engine-loader validation replay (mirrors engine/world/bsp/Bsp.cpp) ─────
struct LoadedHeader {
    bool ok = false;
    bspfmt::BspFileHeader header{};
};

LoadedHeader psybsp_loads_ok(const std::vector<std::uint8_t>& blob) {
    LoadedHeader r;
    if (blob.size() < sizeof(bspfmt::BspFileHeader)) {
        return r;
    }
    std::memcpy(&r.header, blob.data(), sizeof(bspfmt::BspFileHeader));
    const bspfmt::BspFileHeader& h = r.header;
    if (h.magic != bspfmt::kBspFileMagic || h.version != bspfmt::kBspFileVersion) {
        return r;
    }
    if (h.total_bytes > blob.size()) {
        return r;
    }
    const std::uint32_t used = h.total_bytes;
    auto chunk_in_range = [&](const bspfmt::BspFileChunk& c, std::uint32_t elem_size) {
        if (c.count == 0) {
            return true;
        }
        const std::uint64_t bytes = static_cast<std::uint64_t>(c.count) * elem_size;
        return c.offset <= used && bytes <= static_cast<std::uint64_t>(used - c.offset);
    };
    if (!chunk_in_range(h.nodes, sizeof(bspfmt::BspFileNode)) ||
        !chunk_in_range(h.leaves, sizeof(bspfmt::BspFileLeaf)) ||
        !chunk_in_range(h.faces, sizeof(bspfmt::BspFileFace)) ||
        !chunk_in_range(h.vertices, bspfmt::kBspFileVertexBytes) ||
        !chunk_in_range(h.indices, bspfmt::kBspFileIndexBytes)) {
        return r;
    }
    // PVS is a flat byte chunk (count == bytes).
    if (h.pvs.count != 0 && (h.pvs.offset > used || h.pvs.count > used - h.pvs.offset)) {
        return r;
    }
    if (h.cluster_count > 0) {
        const std::uint64_t expected = static_cast<std::uint64_t>(h.cluster_count) * h.pvs_row_bytes;
        if (h.pvs_row_bytes == 0 || expected != h.pvs.count) {
            return r;
        }
    }
    // Topology: every node child resolves to a valid node or leaf.
    std::vector<bspfmt::BspFileNode> nodes(h.nodes.count);
    if (h.nodes.count > 0) {
        std::memcpy(nodes.data(),
                    blob.data() + h.nodes.offset,
                    h.nodes.count * sizeof(bspfmt::BspFileNode));
    }
    const std::int32_t node_count = static_cast<std::int32_t>(h.nodes.count);
    const std::int32_t leaf_count = static_cast<std::int32_t>(h.leaves.count);
    if (node_count > 0 && leaf_count == 0) {
        return r;
    }
    auto child_ok = [&](std::int32_t child) {
        if (bspfmt::bsp_is_leaf(child)) {
            const std::int32_t li = bspfmt::bsp_leaf_index(child);
            return li >= 0 && li < leaf_count;
        }
        return child >= 0 && child < node_count;
    };
    for (const auto& n : nodes) {
        if (!child_ok(n.front_child) || !child_ok(n.back_child)) {
            return r;
        }
    }
    r.ok = true;
    return r;
}

std::string make_temp_path(const char* suffix) {
    static std::atomic<std::uint32_t> counter{0};
    const std::uint32_t n = counter.fetch_add(1, std::memory_order_relaxed);
    const auto pid =
#if defined(_WIN32)
        static_cast<long>(::_getpid());
#else
        static_cast<long>(::getpid());
#endif
    char dirname[64];
    std::snprintf(dirname, sizeof(dirname), "psynder_gx_lmqbsp-%ld", pid);
    const fs::path dir = fs::temp_directory_path() / dirname;
    std::error_code ec;
    fs::create_directories(dir, ec);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "case-%u%s", n, suffix);
    return (dir / buf).string();
}

void write_text_file(const std::string& path, const std::string& body) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    REQUIRE(f.is_open());
    f.write(body.data(), static_cast<std::streamsize>(body.size()));
    REQUIRE(static_cast<bool>(f));
}

std::vector<std::uint8_t> compile_text(const std::string& map_text, psy::lm_qbsp::CompileStats& stats) {
    using namespace psy::lmtools;
    MapFile map;
    MapParseError perr;
    REQUIRE(parse_map(map_text, map, perr));
    REQUIRE(perr.ok);
    psy::lm_qbsp::CompileOptions opts;
    std::vector<std::uint8_t> blob;
    std::string err;
    REQUIRE(psy::lm_qbsp::compile_map(map, opts, blob, &stats, &err));
    return blob;
}

}  // namespace

// ───────────────────────────────────────────────────────────────────────────
// Parser
// ───────────────────────────────────────────────────────────────────────────

TEST_CASE("lm_qbsp: parser reads entity key values and a brush", "[lmqbsp][parse]") {
    using namespace psy::lmtools;
    const std::string text =
        "// a comment\n"
        "{\n"
        "\"classname\" \"worldspawn\"\n"
        "\"message\" \"hello room\"\n" +
        box_brush(-16, -16, -16, 16, 16, 16, "WALL") + "}\n";
    MapFile map;
    MapParseError err;
    REQUIRE(parse_map(text, map, err));
    REQUIRE(map.entities.size() == 1u);
    REQUIRE(map.entities[0].classname() == "worldspawn");
    REQUIRE(map.entities[0].get("message") == "hello room");
    REQUIRE(map.entities[0].brushes.size() == 1u);
    REQUIRE(map.entities[0].brushes[0].faces.size() == 6u);
}

TEST_CASE("lm_qbsp: parse_float is locale-independent and bounds huge exponents",
          "[lmqbsp][parse]") {
    using psy::lmtools::parse_float;
    float v = 0.0f;
    REQUIRE(parse_float("-128", v));
    REQUIRE(v == -128.0f);
    REQUIRE(parse_float("1.5", v));
    REQUIRE(v == 1.5f);
    REQUIRE(parse_float("1e3", v));
    REQUIRE(v == 1000.0f);
    REQUIRE(parse_float("2.5e-2", v));
    REQUIRE(std::fabs(v - 0.025f) < 1e-6f);
    REQUIRE_FALSE(parse_float("1.5x", v));  // trailing garbage
    REQUIRE_FALSE(parse_float("", v));
    REQUIRE_FALSE(parse_float("abc", v));
    // A pathological exponent must saturate rather than spin the loop.
    REQUIRE(parse_float("1e100000000", v));
    REQUIRE(std::isinf(v));
}

TEST_CASE("lm_qbsp: parser accepts Valve-220 texture axes", "[lmqbsp][parse]") {
    using namespace psy::lmtools;
    const std::string text =
        "{\n"
        "\"classname\" \"worldspawn\"\n"
        "{\n"
        "( 0 0 16 ) ( 1 0 16 ) ( 0 1 16 ) BRICK [ 1 0 0 0 ] [ 0 -1 0 0 ] 0 1 1\n"
        "( 0 0 0 ) ( 0 1 0 ) ( 1 0 0 ) BRICK [ 1 0 0 0 ] [ 0 -1 0 0 ] 0 1 1\n"
        "( 0 0 0 ) ( 1 0 0 ) ( 0 0 1 ) BRICK [ 1 0 0 0 ] [ 0 0 -1 0 ] 0 1 1\n"
        "( 0 0 0 ) ( 0 0 1 ) ( 0 1 0 ) BRICK [ 0 1 0 0 ] [ 0 0 -1 0 ] 0 1 1\n"
        "}\n"
        "}\n";
    MapFile map;
    MapParseError err;
    REQUIRE(parse_map(text, map, err));
    REQUIRE(map.entities.size() == 1u);
    REQUIRE(map.entities[0].brushes.size() == 1u);
    REQUIRE(map.entities[0].brushes[0].faces.size() == 4u);
    REQUIRE(map.entities[0].brushes[0].faces[0].valve220);
}

TEST_CASE("lm_qbsp: brush plane normals are oriented outward", "[lmqbsp][geometry]") {
    using namespace psy::lmtools;
    const std::string text =
        "{\n\"classname\" \"worldspawn\"\n" + box_brush(-32, -32, -32, 32, 32, 32, "WALL") + "}\n";
    MapFile map;
    MapParseError err;
    REQUIRE(parse_map(text, map, err));
    const MapBrush& brush = map.entities[0].brushes[0];
    // The box centre must lie behind every (outward) face plane.
    const Vec3 center{0.0f, 0.0f, 0.0f};
    for (const MapFace& f : brush.faces) {
        REQUIRE(plane_distance(f.plane, center) < 0.0f);
    }
}

TEST_CASE("lm_qbsp: brush clips to six quad faces", "[lmqbsp][geometry]") {
    using namespace psy::lmtools;
    const std::string text =
        "{\n\"classname\" \"worldspawn\"\n" + box_brush(-10, -10, -10, 10, 10, 10, "WALL") + "}\n";
    MapFile map;
    MapParseError err;
    REQUIRE(parse_map(text, map, err));
    const std::vector<FacePolygon> polys =
        brush_build_polygons(map.entities[0].brushes[0], map_world_extent(map));
    REQUIRE(polys.size() == 6u);
    for (const FacePolygon& fp : polys) {
        REQUIRE(fp.vertices.size() == 4u);
    }
}

// ───────────────────────────────────────────────────────────────────────────
// Compile + .psybsp
// ───────────────────────────────────────────────────────────────────────────

TEST_CASE("lm_qbsp: hollow room compiles to one cluster and loads", "[lmqbsp][bsp]") {
    psy::lm_qbsp::CompileStats stats;
    const std::vector<std::uint8_t> blob = compile_text(hollow_room(256, 256, 128, 16), stats);

    REQUIRE_FALSE(stats.leaked);
    REQUIRE(stats.brush_count == 6u);
    REQUIRE(stats.cluster_count == 1u);  // single sealed interior
    REQUIRE(stats.empty_leaf_count == 1u);
    REQUIRE(stats.face_count == 6u);  // one inner face per wall slab
    REQUIRE(stats.node_count > 0u);
    REQUIRE(stats.solid_leaf_count > 0u);

    const LoadedHeader loaded = psybsp_loads_ok(blob);
    REQUIRE(loaded.ok);
    REQUIRE(loaded.header.cluster_count == 1u);
    REQUIRE(loaded.header.pvs_row_bytes == 1u);
    REQUIRE(loaded.header.total_bytes == blob.size());
    // The lone cluster sees itself.
    const std::size_t pvs_off = loaded.header.pvs.offset;
    REQUIRE((blob[pvs_off] & 0x01u) != 0u);
}

TEST_CASE("lm_qbsp: vertex slab uses the documented 48-byte stride", "[lmqbsp][format]") {
    psy::lm_qbsp::CompileStats stats;
    const std::vector<std::uint8_t> blob = compile_text(hollow_room(128, 128, 96, 16), stats);
    const LoadedHeader loaded = psybsp_loads_ok(blob);
    REQUIRE(loaded.ok);
    REQUIRE(loaded.header.vertices.count == stats.vertex_count);
    // chunk byte span == vertex_count * 48
    const std::uint32_t span = loaded.header.indices.offset - loaded.header.vertices.offset;
    REQUIRE(span == stats.vertex_count * bspfmt::kBspFileVertexBytes);
    REQUIRE(stats.index_count == stats.face_count * 6u);  // each quad -> 2 tris -> 6 indices
}

TEST_CASE("lm_qbsp: compilation is deterministic", "[lmqbsp][determinism]") {
    const std::string text = hollow_room(200, 160, 112, 16);
    psy::lm_qbsp::CompileStats s0;
    psy::lm_qbsp::CompileStats s1;
    const std::vector<std::uint8_t> a = compile_text(text, s0);
    const std::vector<std::uint8_t> b = compile_text(text, s1);
    REQUIRE(a.size() == b.size());
    REQUIRE(a == b);
}

TEST_CASE("lm_qbsp: two sealed rooms do not see each other in the PVS", "[lmqbsp][pvs]") {
    std::ostringstream o;
    o << "{\n\"classname\" \"worldspawn\"\n";
    // Room A around origin.
    o << box_brush(-128, -128, -64, 128, 128, -48, "FL");
    o << box_brush(-128, -128, 48, 128, 128, 64, "CE");
    o << box_brush(-128, -128, -64, -112, 128, 64, "WE");
    o << box_brush(112, -128, -64, 128, 128, 64, "EA");
    o << box_brush(-128, -128, -64, 128, -112, 64, "SO");
    o << box_brush(-128, 112, -64, 128, 128, 64, "NO");
    // Room B shifted far along +X, fully separate.
    const double dx = 1024;
    o << box_brush(dx - 128, -128, -64, dx + 128, 128, -48, "FL");
    o << box_brush(dx - 128, -128, 48, dx + 128, 128, 64, "CE");
    o << box_brush(dx - 128, -128, -64, dx - 112, 128, 64, "WE");
    o << box_brush(dx + 112, -128, -64, dx + 128, 128, 64, "EA");
    o << box_brush(dx - 128, -128, -64, dx + 128, -112, 64, "SO");
    o << box_brush(dx - 128, 112, -64, dx + 128, 128, 64, "NO");
    o << "}\n";

    psy::lm_qbsp::CompileStats stats;
    const std::vector<std::uint8_t> blob = compile_text(o.str(), stats);
    REQUIRE(stats.cluster_count == 2u);
    REQUIRE(stats.pvs_row_bytes == 1u);

    const LoadedHeader loaded = psybsp_loads_ok(blob);
    REQUIRE(loaded.ok);
    const std::size_t pvs_off = loaded.header.pvs.offset;
    const std::uint8_t row0 = blob[pvs_off + 0];
    const std::uint8_t row1 = blob[pvs_off + 1];
    // Each cluster sees only itself.
    REQUIRE((row0 & 0x01u) != 0u);
    REQUIRE((row0 & 0x02u) == 0u);
    REQUIRE((row1 & 0x02u) != 0u);
    REQUIRE((row1 & 0x01u) == 0u);
}

TEST_CASE("lm_qbsp: file driven compile writes a loadable .psybsp", "[lmqbsp][cli]") {
    const std::string map_path = make_temp_path(".map");
    const std::string out_path = make_temp_path(".psybsp");
    write_text_file(map_path, hollow_room(96, 96, 64, 12));

    psy::lm_qbsp::CompileOptions opts;
    opts.input_path = map_path;
    opts.output_path = out_path;
    opts.force_overwrite = true;
    opts.quiet = true;
    psy::lm_qbsp::CompileStats stats;
    REQUIRE(psy::lm_qbsp::compile(opts, &stats));
    REQUIRE(stats.source_hash != 0u);

    std::ifstream f(out_path, std::ios::binary);
    REQUIRE(f.is_open());
    std::vector<std::uint8_t> blob((std::istreambuf_iterator<char>(f)),
                                   std::istreambuf_iterator<char>());
    REQUIRE(blob.size() == stats.bytes_written);
    REQUIRE(psybsp_loads_ok(blob).ok);
}

TEST_CASE("lm_qbsp: refuses to overwrite without force", "[lmqbsp][safety]") {
    const std::string map_path = make_temp_path(".map");
    const std::string out_path = make_temp_path(".psybsp");
    write_text_file(map_path, hollow_room(64, 64, 48, 8));
    write_text_file(out_path, "stale");

    psy::lm_qbsp::CompileOptions opts;
    opts.input_path = map_path;
    opts.output_path = out_path;
    opts.quiet = true;
    opts.force_overwrite = false;
    REQUIRE_FALSE(psy::lm_qbsp::compile(opts));
    opts.force_overwrite = true;
    REQUIRE(psy::lm_qbsp::compile(opts));
}
