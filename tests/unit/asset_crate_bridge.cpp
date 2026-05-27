// SPDX-License-Identifier: MIT
// Psynder-GX — crate asset bridge tests.
//
// Exercises the existing .obj -> .psymesh cooker path, the runtime
// .psymesh validator, .psymesh -> .lmm repack, and a matching .lmt texture.

#if !__has_include("PsyMeshCook.h")
// psymesh_cook_lib is only present when tools are enabled.
#else

#include "PsyMeshCook.h"
#include "asset/FormatsIo.h"
#include "asset/PsyMeshBridge.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;
namespace fio = psynder::asset::formats;
namespace pmb = psynder::asset::psymesh;

namespace {

std::string make_temp_path(const char* suffix) {
    static std::atomic<std::uint32_t> counter{0};
    const std::uint32_t n = counter.fetch_add(1, std::memory_order_relaxed);
#if defined(_WIN32)
    const auto pid = static_cast<long>(::_getpid());
#else
    const auto pid = static_cast<long>(::getpid());
#endif
    char dirname[80];
    std::snprintf(dirname, sizeof(dirname), "psynder_gx_crate_bridge-%ld", pid);
    const fs::path dir = fs::temp_directory_path() / dirname;
    std::error_code ec;
    fs::create_directories(dir, ec);
    char name[64];
    std::snprintf(name, sizeof(name), "case-%u%s", n, suffix);
    return (dir / name).string();
}

void write_text_file(const std::string& path, const std::string& body) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    REQUIRE(f.is_open());
    f.write(body.data(), static_cast<std::streamsize>(body.size()));
    REQUIRE(static_cast<bool>(f));
}

template <class T>
T read_at(std::span<const psynder::u8> bytes, psynder::usize off) {
    T v{};
    REQUIRE(off + sizeof(T) <= bytes.size());
    std::memcpy(&v, bytes.data() + off, sizeof(T));
    return v;
}

std::vector<psynder::u8> pattern_bytes(psynder::usize n, psynder::u8 seed) {
    std::vector<psynder::u8> v(n);
    for (psynder::usize i = 0; i < n; ++i) {
        v[i] = static_cast<psynder::u8>((i * 37u + seed * 11u + 5u) & 0xFFu);
    }
    return v;
}

const std::string kCrateCubeObj =
    "# textured crate cube, 0.5 metre half-extents\n"
    "vt 0 0\n"
    "vt 1 0\n"
    "vt 1 1\n"
    "vt 0 1\n"
    "vn 0 0 1\n"
    "vn 0 0 -1\n"
    "vn -1 0 0\n"
    "vn 1 0 0\n"
    "vn 0 1 0\n"
    "vn 0 -1 0\n"
    "v -0.5 -0.5 0.5\n"
    "v 0.5 -0.5 0.5\n"
    "v 0.5 0.5 0.5\n"
    "v -0.5 0.5 0.5\n"
    "v 0.5 -0.5 -0.5\n"
    "v -0.5 -0.5 -0.5\n"
    "v -0.5 0.5 -0.5\n"
    "v 0.5 0.5 -0.5\n"
    "v -0.5 -0.5 -0.5\n"
    "v -0.5 -0.5 0.5\n"
    "v -0.5 0.5 0.5\n"
    "v -0.5 0.5 -0.5\n"
    "v 0.5 -0.5 0.5\n"
    "v 0.5 -0.5 -0.5\n"
    "v 0.5 0.5 -0.5\n"
    "v 0.5 0.5 0.5\n"
    "v -0.5 0.5 0.5\n"
    "v 0.5 0.5 0.5\n"
    "v 0.5 0.5 -0.5\n"
    "v -0.5 0.5 -0.5\n"
    "v -0.5 -0.5 -0.5\n"
    "v 0.5 -0.5 -0.5\n"
    "v 0.5 -0.5 0.5\n"
    "v -0.5 -0.5 0.5\n"
    "f 1/1/1 2/2/1 3/3/1\n"
    "f 1/1/1 3/3/1 4/4/1\n"
    "f 5/1/2 6/2/2 7/3/2\n"
    "f 5/1/2 7/3/2 8/4/2\n"
    "f 9/1/3 10/2/3 11/3/3\n"
    "f 9/1/3 11/3/3 12/4/3\n"
    "f 13/1/4 14/2/4 15/3/4\n"
    "f 13/1/4 15/3/4 16/4/4\n"
    "f 17/1/5 18/2/5 19/3/5\n"
    "f 17/1/5 19/3/5 20/4/5\n"
    "f 21/1/6 22/2/6 23/3/6\n"
    "f 21/1/6 23/3/6 24/4/6\n";

}  // namespace

TEST_CASE("asset/crate bridge: psymesh cube repacks to lmm and lmt material",
          "[asset][crate][psymesh][lmm][lmt]") {
    const std::string obj_path = make_temp_path(".obj");
    write_text_file(obj_path, kCrateCubeObj);

    psy::psymesh::CookOptions opts;
    opts.input_kind = psy::psymesh::InputKind::Obj;
    opts.input_path = obj_path;
    opts.compute_tangents = true;
    opts.quiet = true;

    psy::psymesh::CookStats stats;
    std::vector<psynder::u8> psymesh_bytes;
    REQUIRE(psy::psymesh::cook(opts, &stats, &psymesh_bytes));
    REQUIRE(stats.cooked_vertex_count == 24u);
    REQUIRE(stats.index_count == 36u);
    REQUIRE((stats.attr_mask & pmb::kAttrTangent) != 0u);

    pmb::PsyMeshView src;
    REQUIRE(pmb::parse_psymesh(psymesh_bytes.data(), psymesh_bytes.size(), src));
    REQUIRE(src.header.vertex_count == 24u);
    REQUIRE(src.header.index_count == 36u);
    REQUIRE(src.header.submesh_count == 1u);
    REQUIRE(src.header.aabb_min[0] == -0.5f);
    REQUIRE(src.header.aabb_max[2] == 0.5f);

    const psynder::u32 crate_material = fio::material_name_hash("crate");
    std::vector<psynder::u8> lmm_bytes;
    REQUIRE(pmb::build_lmm_from_psymesh(
        src, lmm_bytes, pmb::LmmBridgeOptions{.default_material_hash = crate_material}));

    fio::LmmView mesh;
    REQUIRE(fio::parse_lmm(lmm_bytes.data(), lmm_bytes.size(), mesh));
    REQUIRE(mesh.header.vertex_fmt == fio::LmmVertexFmt::Pos3N3T4UV2);
    REQUIRE(mesh.header.vertex_stride == 48u);
    REQUIRE(mesh.header.vertex_count == 24u);
    REQUIRE(mesh.header.index_count == 36u);
    REQUIRE(mesh.index_stride == 2u);
    REQUIRE(mesh.submesh_count() == 1u);
    REQUIRE(mesh.submesh(0).material_hash == crate_material);
    REQUIRE(mesh.vertices.size() == 24u * 48u);
    REQUIRE(mesh.indices.size() == 36u * 2u);

    REQUIRE(read_at<float>(mesh.vertices, 0) == -0.5f);
    REQUIRE(read_at<float>(mesh.vertices, 4) == -0.5f);
    REQUIRE(read_at<float>(mesh.vertices, 8) == 0.5f);
    REQUIRE(read_at<float>(mesh.vertices, 20) == 1.0f); // normal.z
    REQUIRE(read_at<float>(mesh.vertices, 40) == 0.0f); // uv.u after tangent
    REQUIRE(read_at<float>(mesh.vertices, 44) == 0.0f); // uv.v after tangent
    REQUIRE(read_at<std::uint16_t>(mesh.indices, 0) == 0u);
    REQUIRE(read_at<std::uint16_t>(mesh.indices, 2) == 1u);
    REQUIRE(read_at<std::uint16_t>(mesh.indices, 4) == 2u);

    fio::LmtWriter tex;
    tex.pixel_fmt = fio::LmtPixelFmt::RGBA8;
    tex.flags = fio::kLmtFlagSRGB;
    tex.mips.push_back({2, 2, pattern_bytes(2u * 2u * 4u, 7)});
    tex.mips.push_back({1, 1, pattern_bytes(1u * 1u * 4u, 8)});
    std::vector<psynder::u8> lmt_bytes;
    REQUIRE(tex.build(lmt_bytes));

    fio::LmtView texture;
    REQUIRE(fio::parse_lmt(lmt_bytes.data(), lmt_bytes.size(), texture));
    REQUIRE(texture.header.pixel_fmt == fio::LmtPixelFmt::RGBA8);
    REQUIRE((texture.header.file.flags & fio::kLmtFlagSRGB) != 0u);
    REQUIRE(texture.header.width == 2u);
    REQUIRE(texture.header.height == 2u);
    REQUIRE(texture.mip_count() == 2u);
}

TEST_CASE("asset/crate bridge: psymesh validator rejects bad indices",
          "[asset][crate][psymesh]") {
    const std::string obj_path = make_temp_path(".obj");
    write_text_file(obj_path, kCrateCubeObj);

    psy::psymesh::CookOptions opts;
    opts.input_kind = psy::psymesh::InputKind::Obj;
    opts.input_path = obj_path;
    opts.quiet = true;

    std::vector<psynder::u8> psymesh_bytes;
    REQUIRE(psy::psymesh::cook(opts, nullptr, &psymesh_bytes));

    pmb::PsyMeshView good;
    REQUIRE(pmb::parse_psymesh(psymesh_bytes.data(), psymesh_bytes.size(), good));

    const std::uint32_t bad_index = good.header.vertex_count;
    const psynder::usize index_offset =
        sizeof(pmb::PsyMeshFileHeader) + good.positions.size() + good.normals.size() +
        good.uv0.size() + good.tangents.size() + good.uv1.size() + good.colors.size() +
        good.skin.size();
    std::memcpy(psymesh_bytes.data() + index_offset, &bad_index, sizeof(bad_index));

    pmb::PsyMeshView bad;
    REQUIRE_FALSE(pmb::parse_psymesh(psymesh_bytes.data(), psymesh_bytes.size(), bad));
}

#endif
