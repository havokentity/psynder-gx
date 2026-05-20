// SPDX-License-Identifier: MIT OR Apache-2.0
// Psynder-GX — lane 18 / XOR + RLE snapshot delta codec tests.
//
// Verifies the round-trip property on a synthetic SnapshotFrame-shaped
// byte buffer (the codec operates on opaque bytes so the test uses raw
// fixed-pattern data).

#include <catch2/catch_test_macros.hpp>

#include "net/Snapshot.h"
#include "net/SnapshotDelta.h"

#include <algorithm>
#include <array>
#include <random>
#include <vector>

using namespace psynder;
using namespace psynder::net;

namespace {

std::vector<u8> encode_snapshot_to_bytes(const SnapshotFrame& f) {
    const usize need = kSnapshotHeaderBytes + f.entities.size() * kSnapshotEntityBytes;
    std::vector<u8> buf(need);
    REQUIRE(encode_snapshot(f, std::span<u8>(buf.data(), buf.size())) == need);
    return buf;
}

}  // namespace

TEST_CASE("net: identical snapshots produce a tiny zero-run delta",
          "[net][delta]") {
    SnapshotFrame f{};
    f.tick = 42;
    f.entities.push_back({1, {1.0f, 2.0f, 3.0f}, 0xABCDu});
    f.entities.push_back({2, {4.0f, 5.0f, 6.0f}, 0x1234u});

    const auto bytes = encode_snapshot_to_bytes(f);

    std::vector<u8> delta;
    const usize n = xor_delta_encode(std::span<const u8>(bytes.data(), bytes.size()),
                                     std::span<const u8>(bytes.data(), bytes.size()),
                                     delta);
    CHECK(n == delta.size());
    // All-zeros input compresses to roughly 2 bytes per 256-byte chunk.
    CHECK(delta.size() <= (bytes.size() / 128u + 4u));
}

TEST_CASE("net: XOR delta round-trips with single-entity change",
          "[net][delta][roundtrip]") {
    SnapshotFrame a{}, b{};
    a.tick = 100;
    a.entities.push_back({10, {0.f, 0.f, 0.f}, 0u});
    a.entities.push_back({20, {1.f, 2.f, 3.f}, 0u});
    a.entities.push_back({30, {4.f, 5.f, 6.f}, 0u});
    b = a;
    b.tick = 101;
    b.entities[1].position = {1.5f, 2.5f, 3.5f};
    b.entities[1].state_bits = 0xCAFEu;

    const auto bytes_a = encode_snapshot_to_bytes(a);
    const auto bytes_b = encode_snapshot_to_bytes(b);

    std::vector<u8> delta;
    xor_delta_encode(std::span<const u8>(bytes_a.data(), bytes_a.size()),
                     std::span<const u8>(bytes_b.data(), bytes_b.size()),
                     delta);
    // The delta should be substantially smaller than the full snapshot
    // because only a handful of bytes changed.
    CHECK(delta.size() < bytes_b.size());

    std::vector<u8> recovered;
    REQUIRE(xor_delta_decode(std::span<const u8>(bytes_a.data(), bytes_a.size()),
                             std::span<const u8>(delta.data(), delta.size()),
                             recovered));
    REQUIRE(recovered.size() == bytes_b.size());
    CHECK(std::equal(recovered.begin(), recovered.end(), bytes_b.begin()));

    // And decode actually rebuilds the SnapshotFrame.
    SnapshotFrame decoded{};
    REQUIRE(decode_snapshot(std::span<const u8>(recovered.data(), recovered.size()),
                            decoded));
    CHECK(decoded.tick == b.tick);
    REQUIRE(decoded.entities.size() == b.entities.size());
    CHECK(decoded.entities[1].state_bits == 0xCAFEu);
}

TEST_CASE("net: XOR delta round-trips on randomised payloads",
          "[net][delta][roundtrip][random]") {
    std::mt19937 rng(0xDEADBEEFu);
    std::uniform_int_distribution<int> byte_d(0, 255);

    for (int trial = 0; trial < 8; ++trial) {
        const usize n = static_cast<usize>(256 + (trial * 137) % 1024);
        std::vector<u8> a(n), b(n);
        // Fill a with random; b == a then mutate 5% of bytes.
        for (usize i = 0; i < n; ++i) {
            a[i] = static_cast<u8>(byte_d(rng));
            b[i] = a[i];
        }
        for (usize i = 0; i < n; ++i) {
            if ((byte_d(rng) % 20) == 0) {
                b[i] = static_cast<u8>(byte_d(rng));
            }
        }

        std::vector<u8> delta;
        xor_delta_encode(std::span<const u8>(a.data(), a.size()),
                         std::span<const u8>(b.data(), b.size()), delta);
        std::vector<u8> recovered;
        REQUIRE(xor_delta_decode(std::span<const u8>(a.data(), a.size()),
                                 std::span<const u8>(delta.data(), delta.size()),
                                 recovered));
        REQUIRE(recovered.size() == n);
        CHECK(std::equal(recovered.begin(), recovered.end(), b.begin()));
    }
}

TEST_CASE("net: XOR delta against all-zero baseline equals raw bytes",
          "[net][delta][keyframe]") {
    // Keyframe-equivalent: baseline is zeros, current is arbitrary.
    std::vector<u8> baseline(64, 0u);
    std::vector<u8> current(64);
    for (usize i = 0; i < current.size(); ++i) current[i] = static_cast<u8>(i + 1);

    std::vector<u8> delta;
    xor_delta_encode(std::span<const u8>(baseline.data(), baseline.size()),
                     std::span<const u8>(current.data(), current.size()),
                     delta);
    std::vector<u8> recovered;
    REQUIRE(xor_delta_decode(std::span<const u8>(baseline.data(), baseline.size()),
                             std::span<const u8>(delta.data(), delta.size()),
                             recovered));
    REQUIRE(recovered.size() == current.size());
    CHECK(std::equal(recovered.begin(), recovered.end(), current.begin()));
}
