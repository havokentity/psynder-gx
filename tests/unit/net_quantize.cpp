// SPDX-License-Identifier: MIT OR Apache-2.0
// Psynder-GX — lane 18 / net deterministic fixed-point quantization tests.

#include <array>
#include <cmath>
#include <cstring>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/Types.h"
#include "net/Quantize.h"

using namespace psynder;
using namespace psynder::net;

namespace {

// Round-trip a value at a given resolution and check the reconstruction error
// stays within the documented half-step bound.
void check_roundtrip(f32 v, f32 res) {
    const i32 q   = quantize(v, res);
    const f32 out = dequantize(q, res);
    CHECK(std::fabs(out - v) <= res / 2.f);
}

}  // namespace

TEST_CASE("net-quantize: round-trip stays within half a step", "[net]") {
    const std::array<f32, 7> values = {
        0.f, 1.f, -1.f, 12.3456f, -7.89f, 1234.5678f, -1234.5678f,
    };

    for (f32 v : values) {
        check_roundtrip(v, 0.001f);  // 1 mm
        check_roundtrip(v, 0.01f);   // 1 cm
    }
}

TEST_CASE("net-quantize: round-half-up exactness", "[net]") {
    // floor(x/res + 0.5) ties round toward +inf, not away from zero.
    //   2.5  -> floor(2.5 + 0.5) = floor(3.0)  =  3
    //  -2.5  -> floor(-2.5 + 0.5) = floor(-2.0) = -2
    CHECK(quantize(2.5f, 1.0f) == 3);
    CHECK(quantize(-2.5f, 1.0f) == -2);

    // A few more hand-computed expectations from floor(x/res + 0.5):
    //   3.0 / 1.0 = 3.0 -> floor(3.5) = 3
    //   3.4 / 1.0      -> floor(3.9) = 3
    //   3.6 / 1.0      -> floor(4.1) = 4
    CHECK(quantize(3.0f, 1.0f) == 3);
    CHECK(quantize(3.4f, 1.0f) == 3);
    CHECK(quantize(3.6f, 1.0f) == 4);

    //  -3.6 / 1.0 = -3.6 -> floor(-3.1) = -4
    //  -3.4 / 1.0        -> floor(-2.9) = -3
    CHECK(quantize(-3.6f, 1.0f) == -4);
    CHECK(quantize(-3.4f, 1.0f) == -3);

    //  At 1 mm: 0.1234 / 0.001 = 123.4 -> floor(123.9) = 123
    CHECK(quantize(0.1234f, 0.001f) == 123);
    //  -0.1234 / 0.001 = -123.4 -> floor(-122.9) = -123
    CHECK(quantize(-0.1234f, 0.001f) == -123);
}

TEST_CASE("net-quantize: dequantize maps integer steps back to metres", "[net]") {
    CHECK(dequantize(3, 1.0f) == Catch::Approx(3.0f));
    CHECK(dequantize(123, 0.001f) == Catch::Approx(0.123f));
    CHECK(dequantize(-2, 1.0f) == Catch::Approx(-2.0f));
    CHECK(dequantize(0, 0.01f) == Catch::Approx(0.0f));
}

TEST_CASE("net-quantize: quantize_pos / dequantize_pos round-trip", "[net]") {
    const f32 res = 0.001f;
    const f32 in[3] = {12.345f, -6.789f, 0.5f};

    i32 q[3] = {0, 0, 0};
    quantize_pos(in, res, q);

    // Components match the scalar path exactly.
    CHECK(q[0] == quantize(in[0], res));
    CHECK(q[1] == quantize(in[1], res));
    CHECK(q[2] == quantize(in[2], res));

    f32 out[3] = {0.f, 0.f, 0.f};
    dequantize_pos(q, res, out);

    for (int i = 0; i < 3; ++i) {
        CHECK(std::fabs(out[i] - in[i]) <= res / 2.f);
    }
}

TEST_CASE("net-quantize: invalid resolution guard returns 0", "[net]") {
    CHECK(quantize(42.0f, 0.0f) == 0);
    CHECK(quantize(42.0f, -0.001f) == 0);
    CHECK(quantize(-42.0f, 0.0f) == 0);

    i32 q[3] = {7, 7, 7};
    const f32 in[3] = {1.f, 2.f, 3.f};
    quantize_pos(in, -1.0f, q);
    CHECK(q[0] == 0);
    CHECK(q[1] == 0);
    CHECK(q[2] == 0);
}

TEST_CASE("net-quantize: quantization is deterministic and reproducible",
          "[net][determinism]") {
    // Same input twice -> identical integer.
    CHECK(quantize(987.654f, 0.001f) == quantize(987.654f, 0.001f));

    // Build a thousand-value quantization sequence twice; the integer streams
    // must be bit-identical (lockstep determinism contract).
    const f32 res = 0.001f;
    auto build = [res]() {
        std::vector<i32> out;
        out.reserve(1000);
        for (i32 i = 0; i < 1000; ++i) {
            // Spread across positive and negative, sub-millimetre fractions.
            const f32 v = static_cast<f32>(i - 500) * 0.137f;
            out.push_back(quantize(v, res));
        }
        return out;
    };

    const std::vector<i32> a = build();
    const std::vector<i32> b = build();

    REQUIRE(a.size() == b.size());
    CHECK(a == b);
    CHECK(std::memcmp(a.data(), b.data(), a.size() * sizeof(i32)) == 0);
}
