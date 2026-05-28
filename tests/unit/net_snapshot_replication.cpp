// SPDX-License-Identifier: MIT
// Psynder-GX — lane 18 / net: snapshot delta round-trip on the WIDENED
// EntityState schema (#40, ADR-020). EntityState now carries yaw_deg alongside
// id + pos[3]; these tests prove encode_delta/apply_delta stay correct, minimal,
// and byte-deterministic over the wider record.

#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "net/SnapshotReplication.h"

using namespace psynder;
using namespace psynder::net;

// ── schema guard: widened record is still POD with the expected wire size ───
TEST_CASE("net: widened EntityState wire layout", "[net][replication][schema]") {
    static_assert(std::is_trivially_copyable_v<EntityState>);
    static_assert(sizeof(EntityState) == 20);  // u32 id + 3*f32 pos + f32 yaw
    CHECK(kDeltaRecordBytes == 20);
    CHECK(kDeltaHeaderBytes == 4);
    CHECK(max_delta_size(3) == 4 + 3 * 20);
}

// ── #40 round-trip: encode against a baseline, apply onto a baseline copy ────
TEST_CASE("net: widened delta roundtrip reproduces the mutated set exactly",
          "[net][replication][delta]") {
    std::vector<EntityState> baseline = {
        {1, {0.f, 0.f, 0.f}, 0.f},
        {2, {5.f, 0.f, 0.f}, 90.f},
        {3, {0.f, 9.f, 0.f}, 180.f},
    };
    // current: id 1 unchanged; id 2 moved AND turned; id 3 only turned;
    // id 4 is brand new.
    std::vector<EntityState> current = {
        {1, {0.f, 0.f, 0.f}, 0.f},      // unchanged
        {2, {5.f, 1.f, 0.f}, 45.f},     // pos + yaw changed
        {3, {0.f, 9.f, 0.f}, 270.f},    // yaw-only change still counts
        {4, {7.f, 7.f, 7.f}, 12.f},     // new
    };

    std::vector<u8> delta;
    delta.reserve(max_delta_size(current.size()));
    const usize n = encode_delta(baseline, current, delta);
    REQUIRE(n == delta.size());

    // id 2, 3, 4 are emitted (3 records); id 1 is omitted as unchanged.
    CHECK(delta.size() == kDeltaHeaderBytes + 3 * kDeltaRecordBytes);

    std::vector<EntityState> reconstructed;
    reconstructed.reserve(current.size());
    REQUIRE(apply_delta(baseline, delta, reconstructed));

    REQUIRE(reconstructed.size() == current.size());
    for (const EntityState& want : current) {
        const EntityState* got = find_state(reconstructed, want.id);
        REQUIRE(got != nullptr);
        CHECK(state_equal(*got, want));      // bit-exact pos + yaw
        CHECK(got->yaw_deg == want.yaw_deg);  // explicit yaw check
    }
}

// ── unchanged snapshot encodes to ~zero payload (header only) ───────────────
TEST_CASE("net: identical widened snapshot yields header-only delta",
          "[net][replication][delta]") {
    std::vector<EntityState> baseline = {
        {1, {1.f, 2.f, 3.f}, 15.f},
        {2, {4.f, 5.f, 6.f}, 75.f},
    };
    std::vector<EntityState> current = baseline;  // bit-identical

    std::vector<u8> delta;
    const usize n = encode_delta(baseline, current, delta);
    CHECK(n == kDeltaHeaderBytes);  // count==0, no records

    std::vector<EntityState> out;
    REQUIRE(apply_delta(baseline, delta, out));
    REQUIRE(out.size() == baseline.size());
    for (usize i = 0; i < baseline.size(); ++i) CHECK(state_equal(out[i], baseline[i]));
}

// ── determinism: same inputs -> byte-identical encoding ─────────────────────
TEST_CASE("net: widened delta encoding is byte-identical across runs",
          "[net][replication][delta][determinism]") {
    std::vector<EntityState> baseline = {
        {10, {0.f, 0.f, 0.f}, 0.f},
        {20, {1.f, 1.f, 1.f}, 30.f},
        {30, {2.f, 2.f, 2.f}, 60.f},
    };
    std::vector<EntityState> current = {
        {10, {0.f, 0.f, 0.f}, 5.f},     // yaw moved
        {20, {1.5f, 1.f, 1.f}, 30.f},   // x moved
        {30, {2.f, 2.f, 2.f}, 60.f},    // unchanged
    };

    std::vector<u8> a, b;
    encode_delta(baseline, current, a);
    encode_delta(baseline, current, b);
    REQUIRE(a.size() == b.size());
    CHECK(a == b);  // bit-for-bit identical wire stream
}

// ── yaw-only divergence is detected (no false "unchanged") ──────────────────
TEST_CASE("net: yaw-only change is not omitted as unchanged",
          "[net][replication][delta]") {
    std::vector<EntityState> baseline = {{1, {0.f, 0.f, 0.f}, 0.f}};
    std::vector<EntityState> current  = {{1, {0.f, 0.f, 0.f}, 1.f}};  // yaw differs

    std::vector<u8> delta;
    const usize n = encode_delta(baseline, current, delta);
    CHECK(n == kDeltaHeaderBytes + kDeltaRecordBytes);  // 1 record emitted

    std::vector<EntityState> out;
    REQUIRE(apply_delta(baseline, delta, out));
    const EntityState* got = find_state(out, 1);
    REQUIRE(got != nullptr);
    CHECK(got->yaw_deg == 1.f);
}

// ── truncation guard still holds on the wider record ────────────────────────
TEST_CASE("net: apply_delta rejects a truncated widened stream",
          "[net][replication][delta]") {
    std::vector<EntityState> baseline = {{1, {0.f, 0.f, 0.f}, 0.f}};
    std::vector<EntityState> current  = {{1, {1.f, 0.f, 0.f}, 0.f}};
    std::vector<u8> delta;
    encode_delta(baseline, current, delta);
    REQUIRE(delta.size() == kDeltaHeaderBytes + kDeltaRecordBytes);

    std::span<const u8> truncated(delta.data(), delta.size() - 1);
    std::vector<EntityState> out;
    CHECK_FALSE(apply_delta(baseline, truncated, out));
}
