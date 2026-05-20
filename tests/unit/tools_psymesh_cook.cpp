// SPDX-License-Identifier: MIT
// Psynder-GX — Lane 05 mesh cooker test suite.
//
// Exercises the offline psymesh_cook tool's wire-format contract,
// determinism, --scale / --center-pivot, source-hash staleness,
// and tangent computation.
//
// Drives cook() directly via the psymesh_cook_lib static library
// (no subprocess spawning).
//
// Compiled-out when PSYNDER_GX_BUILD_TOOLS=OFF (psymesh_cook_lib is
// not built in that configuration; we detect it via __has_include
// on our public header).

#if !__has_include("PsyMeshCook.h")
// Stub out the entire file when the cooker library isn't in the link.
// (Catch2 just sees zero psymesh test cases registered.)
#else

#include "PsyMeshCook.h"
#include "PsyMeshFormat.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#if defined(_WIN32)
    #include <process.h>   // _getpid
#else
    #include <unistd.h>    // getpid
#endif

namespace fs = std::filesystem;

using psy::psymesh::CookOptions;
using psy::psymesh::CookStats;
using psy::psymesh::InputKind;
using psy::psymesh::kAttrTangent;
using psy::psymesh::kAttrUv1;
using psy::psymesh::kPsyMeshMagic;
using psy::psymesh::kPsyMeshVersion;
using psy::psymesh::PsyMeshFileHeader;
using psy::psymesh::PsyMeshSubmesh;

// ─────────────────────────────────────────────────────────────────────────
// Static-assert the wire-format constants in test context too, so any
// accidental struct-size drift across compilers shows up here.
// ─────────────────────────────────────────────────────────────────────────
static_assert(sizeof(PsyMeshFileHeader) == 52, "PsyMeshFileHeader is 52 bytes on disk");
static_assert(sizeof(PsyMeshSubmesh)    == 16, "PsyMeshSubmesh is 16 bytes on disk");

namespace {

// Tempfile helper. Catch2 sets up its own temp dir but we just use the
// system temp + a TRULY process-unique counter to keep tests self-
// contained even when ctest + catch_discover_tests run them as separate
// processes in parallel.  Earlier this used only an in-process atomic
// counter, which collided across PIDs sharing the same temp dir
// (Copilot PR #18 review).  Mixing the PID into the directory name
// keeps every test process in its own sub-tree; the in-process counter
// then disambiguates within a single PID.
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
    std::snprintf(dirname, sizeof(dirname), "psynder_gx_psymesh_tests-%ld", pid);
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

// Single-triangle .obj. Right-handed +Y up.
const std::string kSingleTriangleObj =
    "# psymesh_cook test fixture\n"
    "v 0.0 0.0 0.0\n"
    "v 1.0 0.0 0.0\n"
    "v 0.0 1.0 0.0\n"
    "vn 0.0 0.0 1.0\n"
    "vt 0.0 0.0\n"
    "vt 1.0 0.0\n"
    "vt 0.0 1.0\n"
    "f 1/1/1 2/2/1 3/3/1\n";

// Unit cube with verts at ±1 (8 corners, 12 triangles, 36 corners → 8 after dedup).
const std::string kUnitCubeObj =
    "# unit cube ±1\n"
    "v -1 -1 -1\n"
    "v  1 -1 -1\n"
    "v  1  1 -1\n"
    "v -1  1 -1\n"
    "v -1 -1  1\n"
    "v  1 -1  1\n"
    "v  1  1  1\n"
    "v -1  1  1\n"
    // -Z face
    "f 1 3 2\n"
    "f 1 4 3\n"
    // +Z face
    "f 5 6 7\n"
    "f 5 7 8\n"
    // -Y face
    "f 1 2 6\n"
    "f 1 6 5\n"
    // +Y face
    "f 4 8 7\n"
    "f 4 7 3\n"
    // -X face
    "f 1 5 8\n"
    "f 1 8 4\n"
    // +X face
    "f 2 3 7\n"
    "f 2 7 6\n";

// 2-triangle quad in the XY plane with explicit UVs. Used for the
// tangent-unit-length test.
const std::string kQuadObj =
    "v 0 0 0\n"
    "v 1 0 0\n"
    "v 1 1 0\n"
    "v 0 1 0\n"
    "vn 0 0 1\n"
    "vt 0 0\n"
    "vt 1 0\n"
    "vt 1 1\n"
    "vt 0 1\n"
    "f 1/1/1 2/2/1 3/3/1\n"
    "f 1/1/1 3/3/1 4/4/1\n";

// Mesh whose AABB is exactly (0..10) on every axis — for --center-pivot.
const std::string kBox10Obj =
    "v 0  0  0\n"
    "v 10 0  0\n"
    "v 10 10 0\n"
    "v 0  10 0\n"
    "v 0  0  10\n"
    "v 10 0  10\n"
    "v 10 10 10\n"
    "v 0  10 10\n"
    "f 1 3 2\n"
    "f 1 4 3\n"
    "f 5 6 7\n"
    "f 5 7 8\n"
    "f 1 2 6\n"
    "f 1 6 5\n"
    "f 4 8 7\n"
    "f 4 7 3\n"
    "f 1 5 8\n"
    "f 1 8 4\n"
    "f 2 3 7\n"
    "f 2 7 6\n";

// Light-weight reader used to validate the cooked blob's structure
// without depending on a future engine/asset/PsyMeshLoad.h.
struct LoadedPsyMesh {
    PsyMeshFileHeader header;
    std::vector<std::array<float, 3>> positions;
    std::vector<std::array<float, 3>> normals;
    std::vector<std::array<float, 2>> uv0;
    std::vector<std::array<float, 4>> tangents;     // empty unless attr_mask & kAttrTangent
    std::vector<std::array<float, 2>> uv1;          // empty unless attr_mask & kAttrUv1
    std::vector<std::uint32_t>        indices;
    std::vector<PsyMeshSubmesh>       submeshes;
};

LoadedPsyMesh load_blob(const std::vector<std::uint8_t>& blob) {
    LoadedPsyMesh m{};
    REQUIRE(blob.size() >= sizeof(PsyMeshFileHeader));
    std::memcpy(&m.header, blob.data(), sizeof(PsyMeshFileHeader));
    REQUIRE(m.header.magic == kPsyMeshMagic);
    REQUIRE(m.header.version == kPsyMeshVersion);

    std::size_t cursor = sizeof(PsyMeshFileHeader);

    auto read_stream = [&](std::size_t per_vertex_bytes,
                           std::size_t count,
                           void* dst) {
        const std::size_t bytes = per_vertex_bytes * count;
        REQUIRE(cursor + bytes <= blob.size());
        std::memcpy(dst, blob.data() + cursor, bytes);
        cursor += bytes;
    };

    m.positions.resize(m.header.vertex_count);
    read_stream(12, m.header.vertex_count, m.positions.data());
    m.normals.resize(m.header.vertex_count);
    read_stream(12, m.header.vertex_count, m.normals.data());
    m.uv0.resize(m.header.vertex_count);
    read_stream(8, m.header.vertex_count, m.uv0.data());
    if (m.header.attr_mask & kAttrTangent) {
        m.tangents.resize(m.header.vertex_count);
        read_stream(16, m.header.vertex_count, m.tangents.data());
    }
    if (m.header.attr_mask & kAttrUv1) {
        m.uv1.resize(m.header.vertex_count);
        read_stream(8, m.header.vertex_count, m.uv1.data());
    }

    m.indices.resize(m.header.index_count);
    {
        const std::size_t bytes = 4 * m.indices.size();
        REQUIRE(cursor + bytes <= blob.size());
        std::memcpy(m.indices.data(), blob.data() + cursor, bytes);
        cursor += bytes;
    }
    m.submeshes.resize(m.header.submesh_count);
    {
        const std::size_t bytes = sizeof(PsyMeshSubmesh) * m.submeshes.size();
        REQUIRE(cursor + bytes <= blob.size());
        std::memcpy(m.submeshes.data(), blob.data() + cursor, bytes);
        cursor += bytes;
    }
    REQUIRE(cursor == blob.size());
    return m;
}

// Read a small file into memory (used to compare cooker output between
// runs without re-cooking via memory).
std::vector<std::uint8_t> slurp(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    REQUIRE(f.is_open());
    f.seekg(0, std::ios::end);
    const std::streamoff n = f.tellg();
    REQUIRE(n >= 0);
    f.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> out(static_cast<std::size_t>(n));
    if (n > 0) f.read(reinterpret_cast<char*>(out.data()), n);
    REQUIRE(static_cast<bool>(f));
    return out;
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────
// Tests
// ─────────────────────────────────────────────────────────────────────────

TEST_CASE("psymesh: header static-asserts match v1 wire spec", "[psymesh][format]") {
    REQUIRE(sizeof(PsyMeshFileHeader) == 52u);
    REQUIRE(sizeof(PsyMeshSubmesh)    == 16u);
    REQUIRE(kPsyMeshMagic == 0x4D595350u);   // 'P','S','Y','M' LE
    REQUIRE(kPsyMeshVersion == 1u);
}

TEST_CASE("psymesh: single triangle round-trips through cook + re-parse", "[psymesh][cook]") {
    const std::string obj_path = make_temp_path(".obj");
    write_text_file(obj_path, kSingleTriangleObj);

    CookOptions opts;
    opts.input_kind         = InputKind::Obj;
    opts.input_path         = obj_path;
    opts.output_path        = "";          // in-memory only
    opts.compute_tangents   = true;
    opts.force_overwrite    = true;
    opts.quiet              = true;

    std::vector<std::uint8_t> blob;
    CookStats stats;
    REQUIRE(psy::psymesh::cook(opts, &stats, &blob));

    REQUIRE(stats.source_vertex_count == 3u);    // 1 triangle × 3 corners
    REQUIRE(stats.cooked_vertex_count == 3u);    // all unique
    REQUIRE(stats.index_count         == 3u);
    REQUIRE(stats.submesh_count       == 1u);
    REQUIRE((stats.attr_mask & kAttrTangent) != 0u);

    const LoadedPsyMesh m = load_blob(blob);
    REQUIRE(m.positions.size() == 3u);
    REQUIRE(m.indices.size()   == 3u);
    // Each index must be < vertex_count and the index stream must reference
    // all three verts (i.e. the triangle's not collapsed).
    std::vector<std::uint32_t> seen(m.indices.begin(), m.indices.end());
    std::sort(seen.begin(), seen.end());
    REQUIRE(seen == std::vector<std::uint32_t>{ 0u, 1u, 2u });

    // Positions came back in their source order.
    auto find_vert = [&](float x, float y, float z) {
        for (std::size_t i = 0; i < m.positions.size(); ++i) {
            if (m.positions[i][0] == x && m.positions[i][1] == y && m.positions[i][2] == z) return i;
        }
        return static_cast<std::size_t>(-1);
    };
    REQUIRE(find_vert(0, 0, 0) != static_cast<std::size_t>(-1));
    REQUIRE(find_vert(1, 0, 0) != static_cast<std::size_t>(-1));
    REQUIRE(find_vert(0, 1, 0) != static_cast<std::size_t>(-1));
}

TEST_CASE("psymesh: deterministic - same input produces byte-identical output", "[psymesh][determinism]") {
    const std::string obj_path = make_temp_path(".obj");
    write_text_file(obj_path, kUnitCubeObj);

    const std::string out_a = make_temp_path("-a.psymesh");
    const std::string out_b = make_temp_path("-b.psymesh");

    CookOptions opts;
    opts.input_kind       = InputKind::Obj;
    opts.input_path       = obj_path;
    opts.output_path      = out_a;
    opts.compute_tangents = true;
    opts.force_overwrite  = true;
    opts.quiet            = true;
    REQUIRE(psy::psymesh::cook(opts));

    opts.output_path = out_b;
    REQUIRE(psy::psymesh::cook(opts));

    const std::vector<std::uint8_t> a = slurp(out_a);
    const std::vector<std::uint8_t> b = slurp(out_b);
    REQUIRE(a.size() == b.size());
    REQUIRE(a == b);
}

TEST_CASE("psymesh: cube AABB is (-1,-1,-1) .. (+1,+1,+1)", "[psymesh][aabb]") {
    const std::string obj_path = make_temp_path(".obj");
    write_text_file(obj_path, kUnitCubeObj);

    CookOptions opts;
    opts.input_kind       = InputKind::Obj;
    opts.input_path       = obj_path;
    opts.output_path      = "";
    opts.compute_tangents = false;     // not needed here
    opts.force_overwrite  = true;
    opts.quiet            = true;

    std::vector<std::uint8_t> blob;
    CookStats stats;
    REQUIRE(psy::psymesh::cook(opts, &stats, &blob));

    REQUIRE(stats.aabb_min[0] == -1.0f);
    REQUIRE(stats.aabb_min[1] == -1.0f);
    REQUIRE(stats.aabb_min[2] == -1.0f);
    REQUIRE(stats.aabb_max[0] ==  1.0f);
    REQUIRE(stats.aabb_max[1] ==  1.0f);
    REQUIRE(stats.aabb_max[2] ==  1.0f);

    const LoadedPsyMesh m = load_blob(blob);
    REQUIRE(m.header.aabb_min[0] == -1.0f);
    REQUIRE(m.header.aabb_max[0] ==  1.0f);
    // Cube dedups to 8 unique verts (one normal direction per face = 24 ...
    // but here we let the cooker patch flat normals per-triangle, so verts
    // shared across faces with different face-normals are NOT dedup'd —
    // expected count is 24).
    REQUIRE(m.header.vertex_count == 24u);
    REQUIRE(m.header.index_count  == 36u);
}

TEST_CASE("psymesh: --scale 2.0 multiplies all positions", "[psymesh][scale]") {
    const std::string obj_path = make_temp_path(".obj");
    // Mesh with a single vert at (1,0,0).
    write_text_file(obj_path,
        "v 1 0 0\n"
        "v 2 0 0\n"
        "v 1 1 0\n"
        "f 1 2 3\n");

    CookOptions opts;
    opts.input_kind       = InputKind::Obj;
    opts.input_path       = obj_path;
    opts.scale            = 2.0f;
    opts.compute_tangents = false;
    opts.force_overwrite  = true;
    opts.quiet            = true;

    std::vector<std::uint8_t> blob;
    CookStats stats;
    REQUIRE(psy::psymesh::cook(opts, &stats, &blob));

    const LoadedPsyMesh m = load_blob(blob);
    auto find_pos = [&](float x, float y, float z) {
        for (const auto& p : m.positions) {
            if (p[0] == x && p[1] == y && p[2] == z) return true;
        }
        return false;
    };
    REQUIRE(find_pos(2.0f, 0.0f, 0.0f));   // (1,0,0) × 2 → (2,0,0)
    REQUIRE(find_pos(4.0f, 0.0f, 0.0f));
    REQUIRE(find_pos(2.0f, 2.0f, 0.0f));
}

TEST_CASE("psymesh: --center-pivot moves (0..10) AABB to (-5..+5)", "[psymesh][pivot]") {
    const std::string obj_path = make_temp_path(".obj");
    write_text_file(obj_path, kBox10Obj);

    CookOptions opts;
    opts.input_kind       = InputKind::Obj;
    opts.input_path       = obj_path;
    opts.center_pivot     = true;
    opts.compute_tangents = false;
    opts.force_overwrite  = true;
    opts.quiet            = true;

    std::vector<std::uint8_t> blob;
    CookStats stats;
    REQUIRE(psy::psymesh::cook(opts, &stats, &blob));

    REQUIRE(stats.aabb_min[0] == -5.0f);
    REQUIRE(stats.aabb_min[1] == -5.0f);
    REQUIRE(stats.aabb_min[2] == -5.0f);
    REQUIRE(stats.aabb_max[0] ==  5.0f);
    REQUIRE(stats.aabb_max[1] ==  5.0f);
    REQUIRE(stats.aabb_max[2] ==  5.0f);
}

TEST_CASE("psymesh: source_hash differs when input changes by one byte", "[psymesh][staleness]") {
    const std::string a_path = make_temp_path(".obj");
    write_text_file(a_path, kSingleTriangleObj);
    const std::string b_path = make_temp_path(".obj");
    // Swap the last vertex from (0,1,0) to (0,2,0) — single-digit edit
    // that does not change file length but changes a byte.
    std::string mod = kSingleTriangleObj;
    const auto pos = mod.find("v 0.0 1.0 0.0");
    REQUIRE(pos != std::string::npos);
    mod.replace(pos, std::string("v 0.0 1.0 0.0").size(), "v 0.0 2.0 0.0");
    write_text_file(b_path, mod);

    CookOptions opts;
    opts.input_kind       = InputKind::Obj;
    opts.compute_tangents = false;
    opts.force_overwrite  = true;
    opts.quiet            = true;

    CookStats stats_a;
    opts.input_path = a_path;
    REQUIRE(psy::psymesh::cook(opts, &stats_a));

    CookStats stats_b;
    opts.input_path = b_path;
    REQUIRE(psy::psymesh::cook(opts, &stats_b));

    REQUIRE(stats_a.source_hash != stats_b.source_hash);
}

TEST_CASE("psymesh: tangents are computed for a UV-mapped quad and unit length", "[psymesh][tangents]") {
    const std::string obj_path = make_temp_path(".obj");
    write_text_file(obj_path, kQuadObj);

    CookOptions opts;
    opts.input_kind       = InputKind::Obj;
    opts.input_path       = obj_path;
    opts.output_path      = "";
    opts.compute_tangents = true;
    opts.force_overwrite  = true;
    opts.quiet            = true;

    std::vector<std::uint8_t> blob;
    CookStats stats;
    REQUIRE(psy::psymesh::cook(opts, &stats, &blob));

    REQUIRE((stats.attr_mask & kAttrTangent) != 0u);
    REQUIRE_FALSE(stats.tangent_fallback_used);

    const LoadedPsyMesh m = load_blob(blob);
    REQUIRE_FALSE(m.tangents.empty());
    for (const auto& tng : m.tangents) {
        const float lx = tng[0];
        const float ly = tng[1];
        const float lz = tng[2];
        const float len = std::sqrt(lx * lx + ly * ly + lz * lz);
        REQUIRE(std::fabs(len - 1.0f) <= 1e-3f);
        REQUIRE((tng[3] == 1.0f || tng[3] == -1.0f));
    }
}

TEST_CASE("psymesh: quad input rejection - face with 4 corners errors out", "[psymesh][error]") {
    const std::string obj_path = make_temp_path(".obj");
    // 4-corner face — must be rejected.
    write_text_file(obj_path,
        "v 0 0 0\n"
        "v 1 0 0\n"
        "v 1 1 0\n"
        "v 0 1 0\n"
        "f 1 2 3 4\n");

    CookOptions opts;
    opts.input_kind       = InputKind::Obj;
    opts.input_path       = obj_path;
    opts.compute_tangents = false;
    opts.force_overwrite  = true;
    opts.quiet            = true;

    REQUIRE_FALSE(psy::psymesh::cook(opts));
}

TEST_CASE("psymesh: refuses to overwrite without --force", "[psymesh][safety]") {
    const std::string obj_path = make_temp_path(".obj");
    write_text_file(obj_path, kSingleTriangleObj);
    const std::string out_path = make_temp_path(".psymesh");

    // Drop a placeholder file at the output path.
    write_text_file(out_path, "stale");

    CookOptions opts;
    opts.input_kind       = InputKind::Obj;
    opts.input_path       = obj_path;
    opts.output_path      = out_path;
    opts.compute_tangents = false;
    opts.force_overwrite  = false;       // no --force
    opts.quiet            = true;

    REQUIRE_FALSE(psy::psymesh::cook(opts));

    opts.force_overwrite = true;
    REQUIRE(psy::psymesh::cook(opts));
}

TEST_CASE("psymesh: --gen-uv2-lightmap emits UV1 stub mirroring UV0", "[psymesh][uv1]") {
    const std::string obj_path = make_temp_path(".obj");
    write_text_file(obj_path, kQuadObj);

    CookOptions opts;
    opts.input_kind            = InputKind::Obj;
    opts.input_path            = obj_path;
    opts.compute_tangents      = false;
    opts.gen_uv2_lightmap_stub = true;
    opts.force_overwrite       = true;
    opts.quiet                 = true;

    std::vector<std::uint8_t> blob;
    CookStats stats;
    REQUIRE(psy::psymesh::cook(opts, &stats, &blob));

    REQUIRE((stats.attr_mask & kAttrUv1) != 0u);
    const LoadedPsyMesh m = load_blob(blob);
    REQUIRE(m.uv0.size() == m.uv1.size());
    for (std::size_t i = 0; i < m.uv0.size(); ++i) {
        REQUIRE(m.uv0[i][0] == m.uv1[i][0]);
        REQUIRE(m.uv0[i][1] == m.uv1[i][1]);
    }
}

TEST_CASE("psymesh: fnv1a32 matches the canonical 0x811c9dc5 / 0x01000193 spec", "[psymesh][hash]") {
    // Reference vectors from the canonical FNV reference page.
    auto h = [](const char* s) {
        return psy::psymesh::fnv1a32(reinterpret_cast<const std::uint8_t*>(s), std::strlen(s));
    };
    REQUIRE(h("")    == 0x811C9DC5u);
    REQUIRE(h("a")   == 0xE40C292Cu);
    REQUIRE(h("foobar") == 0xBF9CF968u);
}

#endif  // __has_include("PsyMeshCook.h")

