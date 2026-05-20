// SPDX-License-Identifier: MIT
// Psynder-GX - Lane 24 lm_mapimport test suite.
//
// Exercises the .map -> .psimport importer: the wire format, faithful
// round-trip of entities/props/brush-planes/textures, point-entity handling,
// and determinism. Header-only, so this suite includes it through a relative
// path and runs in the default (TOOLS=OFF) test build.

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "../../tools/lm_mapimport/LmMapImport.h"
#include "../../tools/lm_qbsp/MapSource.h"

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace {

// Minimal cursor reader over the .psimport blob.
struct Reader {
    const std::uint8_t* data;
    std::size_t size;
    std::size_t cursor = 0;

    bool need(std::size_t n) const { return cursor + n <= size; }
    std::uint32_t u32() {
        std::uint32_t v = 0;
        REQUIRE(need(4));
        std::memcpy(&v, data + cursor, 4);
        cursor += 4;
        return v;
    }
    std::uint16_t u16() {
        std::uint16_t v = 0;
        REQUIRE(need(2));
        std::memcpy(&v, data + cursor, 2);
        cursor += 2;
        return v;
    }
    float f32() {
        float v = 0.0f;
        REQUIRE(need(4));
        std::memcpy(&v, data + cursor, 4);
        cursor += 4;
        return v;
    }
    std::uint8_t u8() {
        REQUIRE(need(1));
        return data[cursor++];
    }
    std::string str() {
        const std::uint32_t n = u32();
        REQUIRE(need(n));
        std::string s(reinterpret_cast<const char*>(data + cursor), n);
        cursor += n;
        return s;
    }
};

std::string box_brush(double lx, double ly, double lz, double hx, double hy, double hz, const char* tex) {
    std::ostringstream o;
    auto face =
        [&](double ax, double ay, double az, double bx, double by, double bz, double cx, double cy, double cz) {
            o << "( " << ax << " " << ay << " " << az << " ) "
              << "( " << bx << " " << by << " " << bz << " ) "
              << "( " << cx << " " << cy << " " << cz << " ) " << tex << " 0 0 0 1 1\n";
        };
    o << "{\n";
    face(hx, ly, lz, hx, hy, lz, hx, hy, hz);
    face(lx, ly, lz, lx, hy, hz, lx, hy, lz);
    face(lx, hy, lz, hx, hy, lz, hx, hy, hz);
    face(lx, ly, lz, hx, ly, hz, hx, ly, lz);
    face(lx, ly, hz, hx, hy, hz, hx, ly, hz);
    face(lx, ly, lz, hx, ly, lz, hx, hy, lz);
    o << "}\n";
    return o.str();
}

std::string sample_map() {
    std::ostringstream o;
    o << "// sample\n";
    o << "{\n\"classname\" \"worldspawn\"\n\"message\" \"room\"\n";
    o << box_brush(-64, -64, -64, 64, 64, 64, "WALL");
    o << "}\n";
    o << "{\n\"classname\" \"light\"\n\"origin\" \"0 0 32\"\n\"light\" \"300\"\n}\n";
    return o.str();
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
    std::snprintf(dirname, sizeof(dirname), "psynder_gx_lmmap-%ld", pid);
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

}  // namespace

TEST_CASE("lm_mapimport: header is well formed", "[lmmap][format]") {
    using namespace psy::lmtools;
    MapFile map;
    MapParseError perr;
    REQUIRE(parse_map(sample_map(), map, perr));

    psy::lm_mapimport::ImportOptions opts;
    std::vector<std::uint8_t> blob;
    psy::lm_mapimport::ImportStats stats;
    REQUIRE(psy::lm_mapimport::import_map(map, opts, blob, &stats));

    Reader r{blob.data(), blob.size()};
    REQUIRE(r.u32() == psy::lm_mapimport::kPsImportMagic);
    REQUIRE(r.u16() == psy::lm_mapimport::kPsImportVersion);
    REQUIRE(r.u16() == 0u);  // flags
    const std::uint32_t entity_count = r.u32();
    const std::uint32_t payload = r.u32();
    REQUIRE(entity_count == 2u);
    REQUIRE(payload == blob.size() - 16u);
    REQUIRE(stats.entity_count == 2u);
    REQUIRE(stats.brush_count == 1u);
    REQUIRE(stats.face_count == 6u);
    REQUIRE(stats.point_entity_count == 1u);  // the light
}

TEST_CASE("lm_mapimport: round-trips entities brushes and planes faithfully", "[lmmap][roundtrip]") {
    using namespace psy::lmtools;
    MapFile map;
    MapParseError perr;
    REQUIRE(parse_map(sample_map(), map, perr));

    psy::lm_mapimport::ImportOptions opts;
    std::vector<std::uint8_t> blob;
    REQUIRE(psy::lm_mapimport::import_map(map, opts, blob, nullptr));

    Reader r{blob.data(), blob.size()};
    REQUIRE(r.u32() == psy::lm_mapimport::kPsImportMagic);
    r.u16();  // version
    r.u16();  // flags
    const std::uint32_t entity_count = r.u32();
    r.u32();  // payload
    REQUIRE(entity_count == map.entities.size());

    for (std::size_t e = 0; e < entity_count; ++e) {
        const MapEntity& src = map.entities[e];
        const std::uint32_t prop_count = r.u32();
        REQUIRE(prop_count == src.props.size());
        for (std::size_t p = 0; p < prop_count; ++p) {
            const std::string key = r.str();
            const std::string val = r.str();
            REQUIRE(key == src.props[p].first);
            REQUIRE(val == src.props[p].second);
        }
        const std::uint32_t brush_count = r.u32();
        REQUIRE(brush_count == src.brushes.size());
        for (std::size_t b = 0; b < brush_count; ++b) {
            const MapBrush& sb = src.brushes[b];
            const std::uint32_t face_count = r.u32();
            REQUIRE(face_count == sb.faces.size());
            for (std::size_t fi = 0; fi < face_count; ++fi) {
                const MapFace& sf = sb.faces[fi];
                const float nx = r.f32();
                const float ny = r.f32();
                const float nz = r.f32();
                const float d = r.f32();
                REQUIRE(nx == sf.plane.n.x);
                REQUIRE(ny == sf.plane.n.y);
                REQUIRE(nz == sf.plane.n.z);
                REQUIRE(d == sf.plane.d);
                r.u8();  // valve220
                r.u8();
                r.u8();
                r.u8();  // pad
                for (int k = 0; k < 8; ++k) {
                    r.f32();  // u_axis(3)+u_off + v_axis(3)+v_off
                }
                r.f32();  // rotation
                r.f32();  // scale_x
                r.f32();  // scale_y
                const std::string tex = r.str();
                REQUIRE(tex == sf.texture);
            }
        }
    }
    REQUIRE(r.cursor == blob.size());
}

TEST_CASE("lm_mapimport: import is deterministic", "[lmmap][determinism]") {
    using namespace psy::lmtools;
    MapFile map;
    MapParseError perr;
    REQUIRE(parse_map(sample_map(), map, perr));

    psy::lm_mapimport::ImportOptions opts;
    std::vector<std::uint8_t> a;
    std::vector<std::uint8_t> b;
    REQUIRE(psy::lm_mapimport::import_map(map, opts, a, nullptr));
    REQUIRE(psy::lm_mapimport::import_map(map, opts, b, nullptr));
    REQUIRE(a == b);
}

TEST_CASE("lm_mapimport: scale multiplies brush plane distances", "[lmmap][scale]") {
    using namespace psy::lmtools;
    MapFile map;
    MapParseError perr;
    REQUIRE(parse_map(sample_map(), map, perr));

    psy::lm_mapimport::ImportOptions opts;
    opts.scale = 2.0f;
    std::vector<std::uint8_t> blob;
    REQUIRE(psy::lm_mapimport::import_map(map, opts, blob, nullptr));

    // Skip the header + worldspawn props, read the first face's plane.
    Reader r{blob.data(), blob.size()};
    r.u32();  // magic
    r.u16();
    r.u16();
    r.u32();  // entity count
    r.u32();  // payload
    const std::uint32_t prop_count = r.u32();
    for (std::uint32_t p = 0; p < prop_count; ++p) {
        r.str();
        r.str();
    }
    r.u32();  // brush_count
    r.u32();  // face_count
    r.f32();  // nx
    r.f32();  // ny
    r.f32();  // nz
    const float d = r.f32();
    REQUIRE(d == map.entities[0].brushes[0].faces[0].plane.d * 2.0f);
}

TEST_CASE("lm_mapimport: file driven import writes a readable .psimport", "[lmmap][cli]") {
    const std::string map_path = make_temp_path(".map");
    const std::string out_path = make_temp_path(".psimport");
    write_text_file(map_path, sample_map());

    psy::lm_mapimport::ImportOptions opts;
    opts.input_path = map_path;
    opts.output_path = out_path;
    opts.force_overwrite = true;
    opts.quiet = true;
    psy::lm_mapimport::ImportStats stats;
    REQUIRE(psy::lm_mapimport::import(opts, &stats));
    REQUIRE(stats.source_hash != 0u);

    std::ifstream f(out_path, std::ios::binary);
    REQUIRE(f.is_open());
    std::vector<std::uint8_t> blob((std::istreambuf_iterator<char>(f)),
                                   std::istreambuf_iterator<char>());
    REQUIRE(blob.size() == stats.bytes_written);
    Reader r{blob.data(), blob.size()};
    REQUIRE(r.u32() == psy::lm_mapimport::kPsImportMagic);
}
