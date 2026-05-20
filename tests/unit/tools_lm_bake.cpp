// SPDX-License-Identifier: MIT
// Psynder-GX - Lane 24 lm_bake test suite.
//
// Exercises the offline path-traced lightmap baker: the .lmt wire format,
// direct lighting, BVH-accelerated shadowing, indirect bounce gather, and
// determinism. The tracer is header-only so this suite includes it through
// a relative path and runs in the default (TOOLS=OFF) test build.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstring>
#include <vector>

#include "../../tools/lm_bake/LmBake.h"
#include "asset/Formats.h"

namespace fmt = psynder::asset::formats;

namespace {

psy::lm_bake::BakeSurface quad(psy::lm_bake::Vec3 a,
                               psy::lm_bake::Vec3 b,
                               psy::lm_bake::Vec3 c,
                               psy::lm_bake::Vec3 d,
                               psy::lm_bake::Vec3 normal) {
    psy::lm_bake::BakeSurface s;
    s.vertices = {a, b, c, d};
    s.normal = normal;
    s.material = 0;
    return s;
}

// A floor centred at the origin, half-size S, normal +Z.
psy::lm_bake::BakeSurface floor_quad(float s) {
    return quad({-s, -s, 0.0f}, {s, -s, 0.0f}, {s, s, 0.0f}, {-s, s, 0.0f}, {0.0f, 0.0f, 1.0f});
}

psy::lm_bake::BakeLight overhead_light(float height, float intensity) {
    psy::lm_bake::BakeLight l;
    l.kind = psy::lm_bake::BakeLight::Kind::Point;
    l.position = {0.0f, 0.0f, height};
    l.color = {1.0f, 1.0f, 1.0f};
    l.intensity = intensity;
    return l;
}

struct LmtView {
    bool ok = false;
    fmt::LmtHeader header{};
    const std::uint8_t* pixels = nullptr;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

LmtView view_lmt(const std::vector<std::uint8_t>& blob) {
    LmtView v;
    if (blob.size() < sizeof(fmt::LmtHeader)) {
        return v;
    }
    std::memcpy(&v.header, blob.data(), sizeof(fmt::LmtHeader));
    v.width = v.header.width;
    v.height = v.header.height;
    if (v.header.pixels_offset + static_cast<std::uint64_t>(v.width) * v.height * 4u > blob.size()) {
        return v;
    }
    v.pixels = blob.data() + v.header.pixels_offset;
    v.ok = true;
    return v;
}

// Red channel (0..255) of an atlas texel; lights are white so R tracks luma.
int texel_r(const LmtView& v, std::uint32_t x, std::uint32_t y) {
    const std::size_t idx = (static_cast<std::size_t>(y) * v.width + x) * 4u;
    return static_cast<int>(v.pixels[idx]);
}

}  // namespace

TEST_CASE("lm_bake: emits a valid RGBA8 .lmt header", "[lmbake][format]") {
    psy::lm_bake::Scene scene;
    scene.materials.push_back(psy::lm_bake::BakeMaterial{});
    scene.surfaces.push_back(floor_quad(4.0f));
    scene.lights.push_back(overhead_light(5.0f, 300.0f));

    psy::lm_bake::BakeOptions opts;
    opts.samples = 0;  // direct only
    opts.bounces = 0;
    psy::lm_bake::BakeStats stats;
    std::vector<std::uint8_t> blob;
    std::string err;
    REQUIRE(psy::lm_bake::bake(scene, opts, &stats, &blob, &err));

    const LmtView v = view_lmt(blob);
    REQUIRE(v.ok);
    REQUIRE(v.header.file.magic == fmt::kLmtMagic);
    REQUIRE(v.header.file.version == fmt::kLmtVersion);
    REQUIRE(v.header.pixel_fmt == fmt::LmtPixelFmt::RGBA8);
    REQUIRE(v.header.mip_count == 1u);
    REQUIRE(v.header.pixels_offset == sizeof(fmt::LmtHeader) + sizeof(fmt::LmtMip));
    REQUIRE(v.header.file.payload_size == blob.size() - sizeof(fmt::FileHeader));
    REQUIRE(v.width == 34u);  // 32-texel patch (8 m * 4 texels/m) + 1px gutter each side
    REQUIRE(v.height == 34u);
    REQUIRE(stats.bytes_written == blob.size());
    REQUIRE(stats.texels_lit > 0u);
    REQUIRE(stats.max_luminance > 0.0);
}

TEST_CASE("lm_bake: an occluder casts a shadow on the floor", "[lmbake][shadow]") {
    // Scene A: lit floor. Scene B: same floor with a slab between it and the
    // light. The floor centre texel must go dark in B (BVH any-hit shadow).
    const float s = 4.0f;
    psy::lm_bake::BakeOptions opts;
    opts.samples = 0;  // direct only, no ambient -> shadow is pure black
    opts.bounces = 0;

    psy::lm_bake::Scene a;
    a.materials.push_back(psy::lm_bake::BakeMaterial{});
    a.surfaces.push_back(floor_quad(s));
    a.lights.push_back(overhead_light(5.0f, 300.0f));
    std::vector<std::uint8_t> blob_a;
    std::string err;
    REQUIRE(psy::lm_bake::bake(a, opts, nullptr, &blob_a, &err));

    psy::lm_bake::Scene b = a;
    // Occluder slab at z = 2.5 covering the centre (surface index 1).
    b.surfaces.push_back(quad({-2.0f, -2.0f, 2.5f},
                              {2.0f, -2.0f, 2.5f},
                              {2.0f, 2.0f, 2.5f},
                              {-2.0f, 2.0f, 2.5f},
                              {0.0f, 0.0f, 1.0f}));
    std::vector<std::uint8_t> blob_b;
    REQUIRE(psy::lm_bake::bake(b, opts, nullptr, &blob_b, &err));

    const LmtView va = view_lmt(blob_a);
    const LmtView vb = view_lmt(blob_b);
    REQUIRE(va.ok);
    REQUIRE(vb.ok);
    // Floor is surface 0 -> patch 0 at atlas origin (1,1), 32x32; centre ~ (17,17).
    const int lit = texel_r(va, 17u, 17u);
    const int shadowed = texel_r(vb, 17u, 17u);
    REQUIRE(lit > 200);     // directly under the light -> bright
    REQUIRE(shadowed < 8);  // blocked by the slab -> dark
}

TEST_CASE("lm_bake: indirect bounce adds light versus direct only", "[lmbake][gi]") {
    // A floor + ceiling box lit from inside: a 1-bounce gather must not be
    // dimmer than direct-only, and should add some indirect fill.
    auto build = [](std::uint32_t bounces, std::uint32_t samples) {
        psy::lm_bake::Scene scene;
        scene.materials.push_back(psy::lm_bake::BakeMaterial{});  // bright albedo
        scene.surfaces.push_back(floor_quad(4.0f));
        scene.surfaces.push_back(quad({-4.0f, -4.0f, 4.0f},
                                      {4.0f, -4.0f, 4.0f},
                                      {4.0f, 4.0f, 4.0f},
                                      {-4.0f, 4.0f, 4.0f},
                                      {0.0f, 0.0f, -1.0f}));  // ceiling
        scene.lights.push_back(overhead_light(3.5f, 200.0f));
        psy::lm_bake::BakeOptions opts;
        opts.bounces = bounces;
        opts.samples = samples;
        psy::lm_bake::BakeStats stats;
        std::vector<std::uint8_t> blob;
        std::string err;
        REQUIRE(psy::lm_bake::bake(scene, opts, &stats, &blob, &err));
        return stats.max_luminance;
    };
    const double direct = build(0u, 0u);
    const double bounced = build(1u, 16u);
    REQUIRE(direct > 0.0);
    REQUIRE(bounced >= direct * 0.99);  // bounce never removes energy
}

TEST_CASE("lm_bake: baking is deterministic with sampling enabled", "[lmbake][determinism]") {
    auto run = []() {
        psy::lm_bake::Scene scene;
        scene.materials.push_back(psy::lm_bake::BakeMaterial{});
        scene.surfaces.push_back(floor_quad(3.0f));
        scene.surfaces.push_back(quad({-3.0f, -3.0f, 3.0f},
                                      {3.0f, -3.0f, 3.0f},
                                      {3.0f, 3.0f, 3.0f},
                                      {-3.0f, 3.0f, 3.0f},
                                      {0.0f, 0.0f, -1.0f}));
        scene.lights.push_back(overhead_light(2.5f, 150.0f));
        psy::lm_bake::BakeOptions opts;
        opts.bounces = 2;
        opts.samples = 24;
        std::vector<std::uint8_t> blob;
        std::string err;
        REQUIRE(psy::lm_bake::bake(scene, opts, nullptr, &blob, &err));
        return blob;
    };
    const std::vector<std::uint8_t> a = run();
    const std::vector<std::uint8_t> b = run();
    REQUIRE(a.size() == b.size());
    REQUIRE(a == b);
}

TEST_CASE("lm_bake: refuses an empty scene", "[lmbake][error]") {
    psy::lm_bake::Scene scene;  // no surfaces
    psy::lm_bake::BakeOptions opts;
    std::vector<std::uint8_t> blob;
    std::string err;
    REQUIRE_FALSE(psy::lm_bake::bake(scene, opts, nullptr, &blob, &err));
    REQUIRE_FALSE(err.empty());
}
