// SPDX-License-Identifier: MIT
// Psynder-GX — lane 18 / net core scaffold tests: snapshot replication delta
// (#40), interest management (#43), client prediction reconcile (#41).

#include <array>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "net/InterestManagement.h"
#include "net/Prediction.h"
#include "net/SnapshotReplication.h"

using namespace psynder;
using namespace psynder::net;

// ── #40 replication: baseline + delta roundtrip reproduces `current` ────────
TEST_CASE("net: snapshot delta roundtrip reproduces current exactly",
          "[net][replication][delta]") {
    std::vector<EntityState> baseline = {
        {1, {0.f, 0.f, 0.f}},
        {2, {5.f, 0.f, 0.f}},
        {3, {0.f, 9.f, 0.f}},
    };
    // current: id 1 unchanged, id 2 moved, id 3 unchanged, id 4 is new.
    std::vector<EntityState> current = {
        {1, {0.f, 0.f, 0.f}},
        {2, {5.f, 1.f, 0.f}},   // changed
        {3, {0.f, 9.f, 0.f}},
        {4, {7.f, 7.f, 7.f}},   // new
    };

    std::vector<u8> delta;
    delta.reserve(max_delta_size(current.size()));
    const usize n = encode_delta(baseline, current, delta);
    REQUIRE(n == delta.size());

    // Only the changed (id 2) + new (id 4) entities are in the delta:
    // header(4) + 2 records(16 each) = 36 bytes.
    CHECK(delta.size() == kDeltaHeaderBytes + 2 * kDeltaRecordBytes);

    std::vector<EntityState> reconstructed;
    REQUIRE(apply_delta(baseline, delta, reconstructed));

    // Reconstruction must match `current` exactly, id-for-id and bit-for-bit.
    REQUIRE(reconstructed.size() == current.size());
    for (const EntityState& want : current) {
        const EntityState* got = find_state(reconstructed, want.id);
        REQUIRE(got != nullptr);
        CHECK(state_equal(*got, want));
    }
}

TEST_CASE("net: identical snapshot yields an empty (count==0) delta",
          "[net][replication][delta]") {
    std::vector<EntityState> baseline = {{1, {1.f, 2.f, 3.f}}, {2, {4.f, 5.f, 6.f}}};
    std::vector<EntityState> current = baseline;

    std::vector<u8> delta;
    const usize n = encode_delta(baseline, current, delta);
    CHECK(n == kDeltaHeaderBytes);  // header only, no records

    std::vector<EntityState> out;
    REQUIRE(apply_delta(baseline, delta, out));
    REQUIRE(out.size() == baseline.size());
    for (usize i = 0; i < baseline.size(); ++i) CHECK(state_equal(out[i], baseline[i]));
}

TEST_CASE("net: apply_delta rejects a truncated stream",
          "[net][replication][delta]") {
    std::vector<EntityState> baseline = {{1, {0.f, 0.f, 0.f}}};
    std::vector<EntityState> current = {{1, {1.f, 0.f, 0.f}}};
    std::vector<u8> delta;
    encode_delta(baseline, current, delta);
    REQUIRE(delta.size() > kDeltaHeaderBytes);

    // Lop off the last byte: the record now runs past the buffer.
    std::span<const u8> truncated(delta.data(), delta.size() - 1);
    std::vector<EntityState> out;
    CHECK_FALSE(apply_delta(baseline, truncated, out));
}

// ── #43 interest management: AoI returns exactly the in-range ids ───────────
TEST_CASE("net: aoi_query returns exactly the in-range ids",
          "[net][interest][aoi]") {
    std::vector<EntityPos> all = {
        {1, {  0.f,  0.f, 0.f}},  // at center -> in
        {2, { 10.f,  0.f, 0.f}},  // dist 10   -> in
        {3, {  6.f,  8.f, 0.f}},  // dist 10   -> in (boundary, inclusive)
        {4, { 11.f,  0.f, 0.f}},  // dist 11   -> out
        {5, {100.f,100.f, 0.f}},  // far       -> out
    };

    std::vector<u32> ids;
    const usize n = aoi_query(math::Vec3{0, 0, 0}, /*radius=*/10.f, all, ids);
    REQUIRE(n == 3);
    REQUIRE(ids.size() == 3);
    CHECK(ids[0] == 1);
    CHECK(ids[1] == 2);
    CHECK(ids[2] == 3);  // boundary entity included
}

// ── #41 prediction: reconcile replays pending inputs to the expected state ──
TEST_CASE("net: prediction reconcile replays a deterministic mover",
          "[net][prediction][reconcile]") {
    constexpr f32 kDt = 1.0f / 128.0f;  // 128-tick

    InputRing<256> ring;
    // Client applied inputs for ticks 1..5: +1 m/s along +x each tick.
    for (u32 t = 1; t <= 5; ++t) ring.push(Input{t, {1.f, 0.f, 0.f}});

    // Server acked tick 3 with authoritative x = 3 * dt (3 ticks integrated).
    const u32 acked_tick = 3;
    f32 pos_x = 3.0f * kDt;

    std::array<Input, 256> scratch{};
    const usize pend = ring.pending_after(acked_tick, std::span<Input>(scratch));
    REQUIRE(pend == 2);  // ticks 4 and 5 remain

    const usize replayed =
        reconcile(acked_tick, std::span<const Input>(scratch.data(), pend),
                  [&](const Input& in) { pos_x += in.move[0] * kDt; });
    REQUIRE(replayed == 2);

    // After replaying ticks 4 and 5: x == 5 * dt.
    const f32 expected = 5.0f * kDt;
    CHECK(pos_x == expected);  // bit-exact: same +kDt steps, same order
}

TEST_CASE("net: reconcile skips inputs already covered by the ack",
          "[net][prediction][reconcile]") {
    std::array<Input, 3> inputs = {
        Input{2, {1.f, 0.f, 0.f}},
        Input{3, {1.f, 0.f, 0.f}},
        Input{4, {1.f, 0.f, 0.f}},
    };
    u32 steps = 0;
    const usize replayed = reconcile(
        /*last_acked_tick=*/3, std::span<const Input>(inputs.data(), inputs.size()),
        [&](const Input&) { ++steps; });
    CHECK(replayed == 1);  // only tick 4 > 3
    CHECK(steps == 1);
}
