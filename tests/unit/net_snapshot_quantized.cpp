// SPDX-License-Identifier: MIT OR Apache-2.0
// Psynder-GX — lane 18 / net quantized snapshot state tests.

#include <cmath>
#include <cstring>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/Types.h"
#include "net/SnapshotQuantized.h"
#include "net/SnapshotReplication.h"

using namespace psynder;
using namespace psynder::net;

TEST_CASE("net-snapquant: round-trips an entity within the documented bounds",
          "[net]") {
    const f32 res = 0.001f;  // 1 mm position resolution
    EntityState s{};
    s.id = 7;
    s.pos[0] = 12.3456f;
    s.pos[1] = -7.89f;
    s.pos[2] = 0.001f;
    s.yaw_deg = 271.5f;

    const QuantizedState q = quantize_state(s, res);
    const EntityState r = dequantize_state(q, res);

    CHECK(r.id == s.id);
    // Position: each component within half a resolution step.
    CHECK(std::fabs(r.pos[0] - s.pos[0]) <= res / 2.f);
    CHECK(std::fabs(r.pos[1] - s.pos[1]) <= res / 2.f);
    CHECK(std::fabs(r.pos[2] - s.pos[2]) <= res / 2.f);
    // Yaw: within ~0.001 deg (half a milli-degree step, with f32 slack).
    CHECK(std::fabs(r.yaw_deg - s.yaw_deg) <= 0.001f);
}

TEST_CASE("net-snapquant: quantizing the same state is bit-identical",
          "[net][determinism]") {
    const f32 res = 0.001f;
    EntityState s{};
    s.id = 42;
    s.pos[0] = 12.3456f;
    s.pos[1] = -7.89f;
    s.pos[2] = 0.001f;
    s.yaw_deg = 271.5f;

    const QuantizedState a = quantize_state(s, res);
    const QuantizedState b = quantize_state(s, res);

    // Pure integer/double algebra — repeated quantization must be byte-exact.
    CHECK(std::memcmp(&a, &b, sizeof(QuantizedState)) == 0);
}

TEST_CASE("net-snapquant: wire-byte probe matches the struct size", "[net]") {
    CHECK(quantized_wire_bytes() == sizeof(QuantizedState));
}

TEST_CASE("net-snapquant: a clearly-quantized coordinate snaps to its step",
          "[net]") {
    const f32 res = 0.001f;  // 1 mm

    // pos = 1.0007, res = 0.001. quantize does floor(x/res + 0.5):
    //   1.0007 / 0.001 = 1000.7  ->  floor(1000.7 + 0.5) = floor(1001.2) = 1001.
    // dequantize: 1001 * 0.001 = 1.001.
    EntityState s{};
    s.id = 1;
    s.pos[0] = 1.0007f;
    s.pos[1] = 0.f;
    s.pos[2] = 0.f;
    s.yaw_deg = 0.f;

    const QuantizedState q = quantize_state(s, res);
    CHECK(q.pos[0] == 1001);

    const EntityState r = dequantize_state(q, res);
    CHECK(r.pos[0] == Catch::Approx(1.001f).margin(1e-5f));
}
