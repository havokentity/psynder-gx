// SPDX-License-Identifier: MIT OR Apache-2.0
// Psynder-GX — lane 18 / net quantized snapshot wire-pack tests.

#include <cmath>
#include <cstring>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/Types.h"
#include "net/SnapshotPack.h"
#include "net/SnapshotReplication.h"

using namespace psynder;
using namespace psynder::net;

namespace {

// A spread of ids / positions / yaws, including negatives, to exercise the
// signed little-endian record encoding.
std::vector<EntityState> sample_states() {
    std::vector<EntityState> v(5);

    v[0].id = 1;    v[0].pos[0] =  12.345f; v[0].pos[1] = -7.89f;  v[0].pos[2] =  0.001f; v[0].yaw_deg = 271.5f;
    v[1].id = 42;   v[1].pos[0] = -100.25f; v[1].pos[1] =  0.0f;   v[1].pos[2] = 33.333f; v[1].yaw_deg = -45.125f;
    v[2].id = 7;    v[2].pos[0] =   0.0f;   v[2].pos[1] =  0.0f;   v[2].pos[2] =  0.0f;   v[2].yaw_deg =   0.0f;
    v[3].id = 9999; v[3].pos[0] = 1000.5f;  v[3].pos[1] = -1000.5f;v[3].pos[2] = -0.5f;   v[3].yaw_deg = 359.999f;
    v[4].id = 256;  v[4].pos[0] =  -3.14159f;v[4].pos[1] = 2.71828f;v[4].pos[2] = -0.123f;v[4].yaw_deg = -180.0f;

    return v;
}

}  // namespace

TEST_CASE("net-pack: round-trips a set of states within the documented bounds",
          "[net]") {
    const f32 res = 0.001f;  // 1 mm position resolution
    const std::vector<EntityState> in = sample_states();

    std::vector<u8> buf;
    pack_quantized(in, res, buf);

    std::vector<EntityState> out;
    REQUIRE(unpack_quantized(buf, res, out));

    // Same count + order preserved.
    REQUIRE(out.size() == in.size());

    for (usize i = 0; i < in.size(); ++i) {
        CHECK(out[i].id == in[i].id);  // ids exact

        // Position: each component within half a resolution step.
        CHECK(std::fabs(out[i].pos[0] - in[i].pos[0]) <= res / 2.f);
        CHECK(std::fabs(out[i].pos[1] - in[i].pos[1]) <= res / 2.f);
        CHECK(std::fabs(out[i].pos[2] - in[i].pos[2]) <= res / 2.f);

        // Yaw: within ~one milli-degree (half-step plus f32 slack).
        CHECK(out[i].yaw_deg == Catch::Approx(in[i].yaw_deg).margin(0.001));
    }
}

TEST_CASE("net-pack: packed_size matches the actual encoded buffer size",
          "[net]") {
    const f32 res = 0.001f;
    const std::vector<EntityState> in = sample_states();

    std::vector<u8> buf;
    pack_quantized(in, res, buf);

    // 4-byte header + 20 bytes per record.
    CHECK(packed_size(in.size()) == buf.size());
    CHECK(buf.size() == 4u + 20u * in.size());

    // Empty set is just the 4-byte count header.
    std::vector<u8> empty_buf;
    pack_quantized(std::span<const EntityState>{}, res, empty_buf);
    CHECK(empty_buf.size() == packed_size(0));
    CHECK(empty_buf.size() == 4u);
}

TEST_CASE("net-pack: packing the same states twice is byte-identical",
          "[net][determinism]") {
    const f32 res = 0.001f;
    const std::vector<EntityState> in = sample_states();

    std::vector<u8> a;
    std::vector<u8> b;
    pack_quantized(in, res, a);
    pack_quantized(in, res, b);

    REQUIRE(a.size() == b.size());
    CHECK(std::memcmp(a.data(), b.data(), a.size()) == 0);
}

TEST_CASE("net-pack: unpacking a truncated buffer fails",
          "[net]") {
    const f32 res = 0.001f;
    const std::vector<EntityState> in = sample_states();

    std::vector<u8> buf;
    pack_quantized(in, res, buf);
    REQUIRE(buf.size() > 3);

    // First 3 bytes — not even a full u32 count header.
    std::vector<u8> too_short(buf.begin(), buf.begin() + 3);
    std::vector<EntityState> out;
    CHECK_FALSE(unpack_quantized(too_short, res, out));

    // A valid header that claims records the buffer does not contain.
    std::vector<u8> header_only(buf.begin(), buf.begin() + 4);  // count = 5
    CHECK_FALSE(unpack_quantized(header_only, res, out));

    // One byte short of a complete final record.
    std::vector<u8> one_short(buf.begin(), buf.end() - 1);
    CHECK_FALSE(unpack_quantized(one_short, res, out));
}
