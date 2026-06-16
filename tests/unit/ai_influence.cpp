// SPDX-License-Identifier: MIT OR Apache-2.0
//
// tests/unit/ai_influence.cpp — deterministic XZ influence (threat) map: a source
// peaks at its centre and falls off linearly to 0 at its radius, overlapping
// sources sum, the down-gradient flees danger / seeks friendly control while the
// up-gradient seeks it, a flat field has no gradient, and the stamp is
// bit-reproducible.

#include "ai/Influence.h"

#include "math/Math.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace psynder;
using namespace psynder::ai;

namespace {
// XZ dot of two Vec3 (the map ignores y).
f32 dot_xz(math::Vec3 a, math::Vec3 b) { return a.x * b.x + a.z * b.z; }
}  // namespace

TEST_CASE("ai: influence source peaks at centre and falls to zero at radius",
          "[ai]") {
    InfluenceMap m;
    m.resize(11, 11);
    m.add_source(5, 5, 10.0f, 4);

    // Peak at the centre: strength * (1 - 0/4) == strength.
    REQUIRE(m.value(5, 5) == Catch::Approx(10.0f));
    // Halfway out (dist 2 of radius 4): 10 * (1 - 2/4) == 5.
    REQUIRE(m.value(7, 5) == Catch::Approx(5.0f));
    REQUIRE(m.value(5, 3) == Catch::Approx(5.0f));
    // At exactly the radius the cone reaches 0.
    REQUIRE(m.value(9, 5) == Catch::Approx(0.0f).margin(1e-6f));
    // Beyond the radius stays 0.
    REQUIRE(m.value(10, 5) == Catch::Approx(0.0f).margin(1e-6f));
    // A diagonal cell beyond the Euclidean radius is also flat-zero, even though
    // it sits inside the clamped bounding box (max(0, ...) guard).
    REQUIRE(m.value(8, 8) == Catch::Approx(0.0f).margin(1e-6f));
}

TEST_CASE("ai: overlapping sources sum their influence", "[ai]") {
    InfluenceMap m;
    m.resize(11, 11);
    m.add_source(3, 5, 10.0f, 5);
    m.add_source(7, 5, 10.0f, 5);

    // Cell (5,5) is dist 2 from each centre: 10*(1-2/5) == 6 from each, summed.
    REQUIRE(m.value(5, 5) == Catch::Approx(12.0f));
    // Cell (3,5) is source A's centre (full 10) and is dist 4 from source B,
    // which (radius 5) still reaches it: 10*(1-4/5) == 2, so 10 + 2 == 12.
    REQUIRE(m.value(3, 5) == Catch::Approx(12.0f));
}

TEST_CASE("ai: value is zero out of range", "[ai]") {
    InfluenceMap m;
    m.resize(8, 8);
    m.add_source(4, 4, 5.0f, 3);
    REQUIRE(m.value(8, 0) == Catch::Approx(0.0f).margin(1e-6f));
    REQUIRE(m.value(0, 8) == Catch::Approx(0.0f).margin(1e-6f));
    REQUIRE(m.value(100, 100) == Catch::Approx(0.0f).margin(1e-6f));
}

TEST_CASE("ai: down-gradient flees danger, up-gradient seeks it", "[ai]") {
    InfluenceMap m;
    m.resize(21, 21);
    const u32 sx = 10, sz = 10;
    m.add_source(sx, sz, 100.0f, 8);

    // A cell off to one side of the danger source.
    const u32 cx = 14, cz = 10;
    const math::Vec3 from_source{static_cast<f32>(cx) - static_cast<f32>(sx), 0.0f,
                                 static_cast<f32>(cz) - static_cast<f32>(sz)};

    // Down-gradient flees: it points the same way as (cell - source), away.
    const math::Vec3 down = m.down_gradient(cx, cz);
    REQUIRE((down.x != 0.0f || down.z != 0.0f));
    REQUIRE(dot_xz(down, from_source) > 0.0f);

    // Up-gradient seeks: it points back toward the source, opposite (cell-source).
    const math::Vec3 up = m.up_gradient(cx, cz);
    REQUIRE((up.x != 0.0f || up.z != 0.0f));
    REQUIRE(dot_xz(up, from_source) < 0.0f);
}

TEST_CASE("ai: a flat field yields a zero gradient", "[ai]") {
    InfluenceMap m;
    m.resize(8, 8);  // all zeros, never stamped
    const math::Vec3 down = m.down_gradient(4, 4);
    const math::Vec3 up = m.up_gradient(4, 4);
    REQUIRE(down.x == 0.0f);
    REQUIRE(down.z == 0.0f);
    REQUIRE(up.x == 0.0f);
    REQUIRE(up.z == 0.0f);
}

TEST_CASE("ai: a negative-strength control source pulls the down-gradient in",
          "[ai]") {
    InfluenceMap m;
    m.resize(21, 21);
    const u32 sx = 10, sz = 10;
    m.add_source(sx, sz, -100.0f, 8);  // friendly control: a basin, not a peak

    const u32 cx = 14, cz = 10;
    const math::Vec3 from_source{static_cast<f32>(cx) - static_cast<f32>(sx), 0.0f,
                                 static_cast<f32>(cz) - static_cast<f32>(sz)};

    // The lowest influence is now AT the control source, so the down-gradient
    // (toward the lowest neighbour) pulls back toward it: opposite (cell-source).
    const math::Vec3 down = m.down_gradient(cx, cz);
    REQUIRE((down.x != 0.0f || down.z != 0.0f));
    REQUIRE(dot_xz(down, from_source) < 0.0f);
}

TEST_CASE("ai: identical stampings give a bit-identical field", "[ai][determinism]") {
    const auto build = []() {
        InfluenceMap m;
        m.resize(24, 24);
        m.add_source(6, 8, 50.0f, 7);
        m.add_source(17, 4, -30.0f, 9);
        m.add_source(11, 19, 80.0f, 5);
        std::vector<f32> out;
        out.reserve(24u * 24u);
        for (u32 z = 0; z < 24; ++z)
            for (u32 x = 0; x < 24; ++x) out.push_back(m.value(x, z));
        return out;
    };
    const std::vector<f32> a = build();
    const std::vector<f32> b = build();
    REQUIRE(a.size() == b.size());
    for (usize i = 0; i < a.size(); ++i) REQUIRE(a[i] == b[i]);  // exact, bit-for-bit
}

TEST_CASE("ai: add_source is order-independent", "[ai][determinism]") {
    InfluenceMap p;
    p.resize(16, 16);
    p.add_source(4, 4, 10.0f, 6);
    p.add_source(11, 9, -7.5f, 5);

    InfluenceMap q;
    q.resize(16, 16);
    q.add_source(11, 9, -7.5f, 5);  // reversed stamping order
    q.add_source(4, 4, 10.0f, 6);

    for (u32 z = 0; z < 16; ++z)
        for (u32 x = 0; x < 16; ++x) REQUIRE(p.value(x, z) == q.value(x, z));
}
