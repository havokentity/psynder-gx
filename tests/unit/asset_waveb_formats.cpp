// SPDX-License-Identifier: MIT
// Psynder — Lane 05 asset Wave-B tests: byte-exact cook/write/read round
// trips for the .lmm (mesh), .lmt (texture) and .lma (audio) containers,
// plus transparent zstd round-trip through the VFS .lmpak path.
//
// TEST_CASE names are ASCII-only on purpose (see AGENTS.md / tests/unit
// CMakeLists.txt): ctest replays each name as a command-line filter and a
// non-ASCII name is mangled by the Windows CRT argv decoder.

#include "asset/Formats.h"
#include "asset/FormatsIo.h"
#include "asset/LmpakWriter.h"
#include "asset/Vfs.h"
#include "asset/VfsInternal.h"

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace psynder;
using psynder::asset::Vfs;
namespace fio = psynder::asset::formats;

namespace {

// Deterministic byte pattern so round-trips verify position, not just size.
std::vector<u8> pattern_bytes(usize n, u8 seed) {
    std::vector<u8> v(n);
    for (usize i = 0; i < n; ++i) {
        v[i] = static_cast<u8>((i * 131u + static_cast<usize>(seed) * 17u + 7u) & 0xFFu);
    }
    return v;
}

bool span_equals(std::span<const u8> a, const std::vector<u8>& b) {
    return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), b.size()) == 0);
}

fs::path make_scratch_dir(const char* tag) {
    static int counter = 0;
    fs::path base = fs::temp_directory_path() / "psynder_asset_waveb";
    fs::create_directories(base);
    fs::path d = base / (std::string(tag) + "_" + std::to_string(++counter));
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}

struct ResetGuard {
    ResetGuard() { psynder::asset::internal::reset_for_tests(); }
    ~ResetGuard() { psynder::asset::internal::reset_for_tests(); }
};

}  // namespace

TEST_CASE("asset/formats: .lmm mesh round-trips byte-for-byte", "[asset][formats][lmm]") {
    fio::LmmWriter w;
    w.vertex_fmt = fio::LmmVertexFmt::Pos3N3T4UV2;  // 48-byte stride
    w.vertex_count = 12;
    w.index_count = 18;
    w.vertex_data = pattern_bytes(w.vertex_count * fio::lmm_vertex_stride(w.vertex_fmt), 1);
    w.index_data = pattern_bytes(w.index_count * 2, 2);  // <=65535 verts -> 16-bit
    w.bbox_min[0] = -1.5f;
    w.bbox_min[1] = -2.0f;
    w.bbox_min[2] = -0.25f;
    w.bbox_max[0] = 3.0f;
    w.bbox_max[1] = 2.0f;
    w.bbox_max[2] = 0.75f;
    w.submeshes.push_back(fio::LmmSubmesh{0, 9, 0xABCDEF01u, 0});
    w.submeshes.push_back(fio::LmmSubmesh{9, 9, 0x12345678u, 0});

    std::vector<u8> bytes;
    REQUIRE(w.build(bytes));

    // build() is deterministic: a second build yields identical bytes.
    std::vector<u8> bytes2;
    REQUIRE(w.build(bytes2));
    REQUIRE(bytes == bytes2);

    fio::LmmView v;
    REQUIRE(fio::parse_lmm(bytes.data(), bytes.size(), v));
    REQUIRE(v.valid);
    REQUIRE(v.header.file.magic == fio::kLmmMagic);
    REQUIRE(v.header.file.version == fio::kLmmVersion);
    REQUIRE(v.header.file.payload_size == bytes.size() - sizeof(fio::FileHeader));
    REQUIRE(v.header.vertex_count == 12);
    REQUIRE(v.header.index_count == 18);
    REQUIRE(v.header.vertex_stride == 48);
    REQUIRE(v.index_stride == 2);
    REQUIRE(v.header.bbox_min[0] == -1.5f);
    REQUIRE(v.header.bbox_max[2] == 0.75f);

    REQUIRE(v.submeshes.size() == 2);
    REQUIRE(v.submeshes[1].index_start == 9);
    REQUIRE(v.submeshes[1].material_hash == 0x12345678u);
    REQUIRE(span_equals(v.vertices, w.vertex_data));
    REQUIRE(span_equals(v.indices, w.index_data));

    // Reconstruct from the parsed view and confirm the bytes are recoverable.
    fio::LmmWriter rebuild;
    rebuild.vertex_fmt = v.header.vertex_fmt;
    rebuild.vertex_count = v.header.vertex_count;
    rebuild.index_count = v.header.index_count;
    rebuild.vertex_data.assign(v.vertices.begin(), v.vertices.end());
    rebuild.index_data.assign(v.indices.begin(), v.indices.end());
    rebuild.submeshes.assign(v.submeshes.begin(), v.submeshes.end());
    std::memcpy(rebuild.bbox_min, v.header.bbox_min, sizeof(rebuild.bbox_min));
    std::memcpy(rebuild.bbox_max, v.header.bbox_max, sizeof(rebuild.bbox_max));
    std::vector<u8> rebytes;
    REQUIRE(rebuild.build(rebytes));
    REQUIRE(rebytes == bytes);
}

TEST_CASE("asset/formats: .lmm picks 32-bit indices past 65535 verts",
          "[asset][formats][lmm]") {
    REQUIRE(fio::lmm_index_stride(65535) == 2);
    REQUIRE(fio::lmm_index_stride(65536) == 4);

    fio::LmmWriter w;
    w.vertex_fmt = fio::LmmVertexFmt::Pos3N3UV2;  // 32-byte stride
    w.vertex_count = 65536;                       // just over the 16-bit boundary
    w.index_count = 3;
    w.vertex_data = pattern_bytes(w.vertex_count * 32u, 5);  // ~2 MiB, transient
    w.index_data = pattern_bytes(w.index_count * 4u, 6);     // 32-bit indices

    std::vector<u8> bytes;
    REQUIRE(w.build(bytes));

    fio::LmmView v;
    REQUIRE(fio::parse_lmm(bytes.data(), bytes.size(), v));
    REQUIRE(v.index_stride == 4);
    REQUIRE(v.vertices.size() == w.vertex_data.size());
    REQUIRE(span_equals(v.indices, w.index_data));
}

TEST_CASE("asset/formats: .lmt texture round-trips (RGBA8 mipchain + P8 palette)",
          "[asset][formats][lmt]") {
    SECTION("RGBA8 with a 3-level mip chain") {
        fio::LmtWriter w;
        w.pixel_fmt = fio::LmtPixelFmt::RGBA8;
        w.flags = fio::kLmtFlagSRGB;
        w.mips.push_back({4, 4, pattern_bytes(4 * 4 * 4, 10)});
        w.mips.push_back({2, 2, pattern_bytes(2 * 2 * 4, 11)});
        w.mips.push_back({1, 1, pattern_bytes(1 * 1 * 4, 12)});

        std::vector<u8> bytes;
        REQUIRE(w.build(bytes));

        fio::LmtView v;
        REQUIRE(fio::parse_lmt(bytes.data(), bytes.size(), v));
        REQUIRE(v.header.file.magic == fio::kLmtMagic);
        REQUIRE(v.header.width == 4);
        REQUIRE(v.header.height == 4);
        REQUIRE(v.header.mip_count == 3);
        REQUIRE(v.header.palette_offset == 0);
        REQUIRE(v.palette.empty());
        REQUIRE(v.mips.size() == 3);
        REQUIRE(v.header.pixels_offset == v.mips[0].offset);
        REQUIRE(span_equals(v.mip_pixels(0), w.mips[0].pixels));
        REQUIRE(span_equals(v.mip_pixels(1), w.mips[1].pixels));
        REQUIRE(span_equals(v.mip_pixels(2), w.mips[2].pixels));
    }

    SECTION("P8 paletted carries a 1024-byte palette") {
        fio::LmtWriter w;
        w.pixel_fmt = fio::LmtPixelFmt::P8;
        w.palette = pattern_bytes(fio::kLmtPaletteBytes, 20);
        w.mips.push_back({2, 2, pattern_bytes(2 * 2 * 1, 21)});
        w.mips.push_back({1, 1, pattern_bytes(1 * 1 * 1, 22)});

        std::vector<u8> bytes;
        REQUIRE(w.build(bytes));

        fio::LmtView v;
        REQUIRE(fio::parse_lmt(bytes.data(), bytes.size(), v));
        REQUIRE(v.header.pixel_fmt == fio::LmtPixelFmt::P8);
        REQUIRE(v.header.palette_offset != 0);
        REQUIRE(span_equals(v.palette, w.palette));
        REQUIRE(span_equals(v.mip_pixels(0), w.mips[0].pixels));
        REQUIRE(span_equals(v.mip_pixels(1), w.mips[1].pixels));
    }

    SECTION("a palette on a non-paletted format is rejected") {
        fio::LmtWriter w;
        w.pixel_fmt = fio::LmtPixelFmt::RGBA8;
        w.palette = pattern_bytes(fio::kLmtPaletteBytes, 30);  // illegal
        w.mips.push_back({1, 1, pattern_bytes(4, 31)});
        std::vector<u8> bytes;
        REQUIRE_FALSE(w.build(bytes));
    }
}

TEST_CASE("asset/formats: .lma audio round-trips uncompressed PCM with loop",
          "[asset][formats][lma]") {
    fio::LmaWriter w;
    w.sample_rate = 44100;
    w.sample_fmt = fio::LmaSampleFmt::PCM_S16;
    w.channels = 2;
    w.loop = true;
    w.loop_start = 16;
    w.loop_end = 96;
    // 128 stereo S16 frames = 128 * 2ch * 2 bytes.
    w.pcm = pattern_bytes(128 * 2 * 2, 40);

    std::vector<u8> bytes;
    REQUIRE(w.build(bytes));

    fio::LmaView v;
    REQUIRE(fio::parse_lma(bytes.data(), bytes.size(), v));
    REQUIRE(v.header.file.magic == fio::kLmaMagic);
    REQUIRE(v.header.sample_rate == 44100);
    REQUIRE(v.header.channels == 2);
    REQUIRE(v.header.frame_count == 128);
    REQUIRE((v.header.file.flags & fio::kLmaFlagLoop) != 0);
    REQUIRE((v.header.file.flags & fio::kLmaFlagZstd) == 0);
    REQUIRE(v.header.loop_start == 16);
    REQUIRE(v.header.loop_end == 96);
    REQUIRE_FALSE(v.zstd);
    REQUIRE(v.decoded_bytes() == w.pcm.size());

    std::vector<u8> pcm;
    REQUIRE(v.decode(pcm));
    REQUIRE(pcm == w.pcm);
}

TEST_CASE("asset/formats: .lma audio round-trips through zstd",
          "[asset][formats][lma][zstd]") {
    if (!asset::lmpak::zstd_available()) {
        SUCCEED("zstd not built into psynder_asset; compressed .lma path skipped");
        return;
    }
    fio::LmaWriter w;
    w.sample_fmt = fio::LmaSampleFmt::PCM_F32;
    w.channels = 1;
    w.compress = true;
    w.zstd_level = 9;
    // Highly compressible payload so the stored frame is smaller than the PCM.
    w.pcm = std::vector<u8>(48000 * 1 * 4, u8{0});
    for (usize i = 0; i < w.pcm.size(); i += 64) w.pcm[i] = static_cast<u8>(i);

    std::vector<u8> bytes;
    REQUIRE(w.build(bytes));

    fio::LmaView v;
    REQUIRE(fio::parse_lma(bytes.data(), bytes.size(), v));
    REQUIRE(v.zstd);
    REQUIRE((v.header.file.flags & fio::kLmaFlagZstd) != 0);
    REQUIRE(v.stored.size() < w.pcm.size());  // actually compressed
    REQUIRE(v.decoded_bytes() == w.pcm.size());

    std::vector<u8> pcm;
    REQUIRE(v.decode(pcm));
    REQUIRE(pcm == w.pcm);
}

TEST_CASE("asset/formats: .lma edge cases (empty audio, truncated zstd)",
          "[asset][formats][lma]") {
    SECTION("empty audio decodes to nothing") {
        fio::LmaWriter w;
        w.sample_fmt = fio::LmaSampleFmt::PCM_S16;
        w.channels = 1;
        w.pcm = {};  // zero frames
        std::vector<u8> bytes;
        REQUIRE(w.build(bytes));
        fio::LmaView v;
        REQUIRE(fio::parse_lma(bytes.data(), bytes.size(), v));
        REQUIRE(v.header.frame_count == 0);
        REQUIRE(v.decoded_bytes() == 0);
        std::vector<u8> pcm;
        REQUIRE(v.decode(pcm));  // must not dereference a null dst
        REQUIRE(pcm.empty());
    }

    SECTION("a zstd-flagged file with no payload is rejected at parse") {
        fio::LmaHeader h{};
        h.file.magic = fio::kLmaMagic;
        h.file.version = fio::kLmaVersion;
        h.file.flags = fio::kLmaFlagZstd;
        h.file.payload_size = sizeof(fio::LmaHeader) - sizeof(fio::FileHeader);
        h.sample_rate = 48000;
        h.frame_count = 8;  // claims 8 frames but carries no zstd frame
        h.sample_fmt = fio::LmaSampleFmt::PCM_S16;
        h.channels = 1;
        std::vector<u8> bytes(sizeof(fio::LmaHeader), u8{0});
        std::memcpy(bytes.data(), &h, sizeof(h));
        fio::LmaView v;
        REQUIRE_FALSE(fio::parse_lma(bytes.data(), bytes.size(), v));
    }
}

TEST_CASE("asset/formats: parsers reject malformed input", "[asset][formats]") {
    // Build one good .lmt to mutate.
    fio::LmtWriter w;
    w.pixel_fmt = fio::LmtPixelFmt::RGBA8;
    w.mips.push_back({2, 2, pattern_bytes(2 * 2 * 4, 50)});
    std::vector<u8> good;
    REQUIRE(w.build(good));

    fio::LmtView v;

    SECTION("null / truncated buffers fail") {
        REQUIRE_FALSE(fio::parse_lmt(nullptr, good.size(), v));
        REQUIRE_FALSE(fio::parse_lmt(good.data(), sizeof(fio::LmtHeader) - 1, v));
        REQUIRE_FALSE(fio::parse_lmt(good.data(), good.size() - 1, v));  // payload_size mismatch
    }
    SECTION("wrong magic fails") {
        std::vector<u8> bad = good;
        bad[0] ^= 0xFFu;
        REQUIRE_FALSE(fio::parse_lmt(bad.data(), bad.size(), v));
    }
    SECTION("cross-format parse fails on magic") {
        fio::LmmView mv;
        REQUIRE_FALSE(fio::parse_lmm(good.data(), good.size(), mv));
    }
}

TEST_CASE("asset/formats: cooked .lmt round-trips through the VFS .lmpak path",
          "[asset][formats][vfs][lmpak]") {
    ResetGuard rg;

    // Cook a small texture.
    fio::LmtWriter w;
    w.pixel_fmt = fio::LmtPixelFmt::RGBA8;
    w.flags = fio::kLmtFlagSRGB;
    w.mips.push_back({8, 8, pattern_bytes(8 * 8 * 4, 60)});
    w.mips.push_back({4, 4, pattern_bytes(4 * 4 * 4, 61)});
    std::vector<u8> cooked;
    REQUIRE(w.build(cooked));

    auto dir = make_scratch_dir("lmt_in_pak");
    auto pak = dir / "tex.lmpak";

    const bool compress = asset::lmpak::zstd_available();
    {
        asset::lmpak::Writer pw;
        REQUIRE(pw.add_file("textures/wall.lmt", cooked.data(), cooked.size(), compress, 9));
        REQUIRE(pw.write(pak.string().c_str()));
    }

    REQUIRE(Vfs::Get().mount_pak(pak.string()));
    asset::Blob b = Vfs::Get().read("textures/wall.lmt");
    REQUIRE(b.data != nullptr);
    REQUIRE(b.bytes == cooked.size());  // transparent decompress restores exact bytes

    // The bytes served by the VFS parse back to the same texture.
    fio::LmtView v;
    REQUIRE(fio::parse_lmt(b.data, b.bytes, v));
    REQUIRE(v.header.width == 8);
    REQUIRE(v.mips.size() == 2);
    REQUIRE(span_equals(v.mip_pixels(0), w.mips[0].pixels));
    REQUIRE(span_equals(v.mip_pixels(1), w.mips[1].pixels));
}
