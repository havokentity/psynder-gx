// SPDX-License-Identifier: MIT
// Psynder-GX — lane 18 / net core scaffold tests: snapshot replication delta
// (#40), interest management (#43), client prediction reconcile (#41).

#include <algorithm>
#include <array>
#include <random>
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

// ── #43 interest management: broadphase InterestSet matches the oracle ──────

// Sorted brute-force oracle: the id-sorted set the broadphase must reproduce.
static std::vector<u32> brute_sorted(math::Vec3 center, f32 radius,
                                     std::span<const EntityPos> all) {
    std::vector<u32> ids;
    aoi_query(center, radius, all, ids);  // free-function brute-force scan
    std::sort(ids.begin(), ids.end());
    return ids;
}

TEST_CASE("net: InterestSet broadphase matches the brute-force oracle exactly",
          "[net][interest][aoi][broadphase]") {
    InterestSet set;
    set.rebuild({});  // empty roster -> empty result
    std::vector<u32> ids;
    CHECK(set.aoi_query(math::Vec3{0, 0, 0}, 10.f, ids) == 0);

    std::vector<EntityPos> all = {
        {1, {  0.f,  0.f, 0.f}},  // at center -> in
        {2, { 10.f,  0.f, 0.f}},  // dist 10   -> in
        {3, {  6.f,  8.f, 0.f}},  // dist 10   -> in (boundary, inclusive)
        {4, { 11.f,  0.f, 0.f}},  // dist 11   -> out
        {5, {100.f,100.f, 0.f}},  // far       -> out
    };
    set.rebuild(all);
    const usize n = set.aoi_query(math::Vec3{0, 0, 0}, /*radius=*/10.f, ids);
    REQUIRE(n == 3);
    CHECK(ids == brute_sorted(math::Vec3{0, 0, 0}, 10.f, all));  // {1,2,3}
}

TEST_CASE("net: InterestSet boundary (distance == radius) matches the oracle",
          "[net][interest][aoi][broadphase][boundary]") {
    std::vector<EntityPos> all = {
        {1, {6.f, 8.f, 0.f}},      // exactly radius 10 -> inclusive -> in
        {2, {6.f, 8.0001f, 0.f}},  // one ULP past 10   -> out
    };
    InterestSet set;
    set.rebuild(all);
    std::vector<u32> ids;
    const usize n = set.aoi_query(math::Vec3{0, 0, 0}, /*radius=*/10.f, ids);
    CHECK(ids == brute_sorted(math::Vec3{0, 0, 0}, 10.f, all));
    REQUIRE(n == 1);
    CHECK(ids[0] == 1);  // boundary entity in, ULP-past entity out
}

TEST_CASE("net: InterestSet equals the oracle over seeded random distributions",
          "[net][interest][aoi][broadphase][parity]") {
    std::mt19937 rng(0xA01Fu);
    std::uniform_real_distribution<f32> coord(-500.f, 500.f);

    for (u32 trial = 0; trial < 16; ++trial) {
        const usize count = 64 + (rng() % 256);
        std::vector<EntityPos> all;
        all.reserve(count);
        for (usize i = 0; i < count; ++i)
            all.push_back(EntityPos{static_cast<u32>(i + 1),
                                    math::Vec3{coord(rng), coord(rng), coord(rng)}});

        InterestSet set;
        set.rebuild(all);

        // A spread of query centres + radii, including very large and tiny.
        for (u32 q = 0; q < 8; ++q) {
            const math::Vec3 center{coord(rng), coord(rng), coord(rng)};
            const f32 radius = 1.f + (static_cast<f32>(rng() % 1200));
            std::vector<u32> got;
            set.aoi_query(center, radius, got);
            CHECK(got == brute_sorted(center, radius, all));
        }
    }
}

TEST_CASE("net: InterestSet refit tracks moved positions and matches the oracle",
          "[net][interest][aoi][broadphase][refit]") {
    std::vector<EntityPos> all = {
        {1, {1000.f, 0.f, 0.f}},  // far outside
        {2, {1000.f, 0.f, 0.f}},  // far outside
        {3, {1000.f, 0.f, 0.f}},  // far outside
    };
    InterestSet set;
    set.rebuild(all);
    std::vector<u32> ids;
    set.aoi_query(math::Vec3{0, 0, 0}, 5.f, ids);
    CHECK(ids.empty());

    // Move two into range in place; same roster -> cheap refit path.
    all[0].pos = math::Vec3{1.f, 0.f, 0.f};
    all[2].pos = math::Vec3{0.f, 2.f, 0.f};
    set.refit(all);
    set.aoi_query(math::Vec3{0, 0, 0}, 5.f, ids);
    CHECK(ids == brute_sorted(math::Vec3{0, 0, 0}, 5.f, all));  // {1,3}
}

TEST_CASE("net: InterestSet query is deterministic across repeated calls",
          "[net][interest][aoi][broadphase][determinism]") {
    std::mt19937 rng(0xDE7Eu);
    std::uniform_real_distribution<f32> coord(-300.f, 300.f);
    std::vector<EntityPos> all;
    for (usize i = 0; i < 500; ++i)
        all.push_back(EntityPos{static_cast<u32>(i + 1),
                                math::Vec3{coord(rng), coord(rng), coord(rng)}});
    InterestSet set;
    set.rebuild(all);

    std::vector<u32> first;
    set.aoi_query(math::Vec3{10.f, -20.f, 5.f}, 150.f, first);
    for (u32 rep = 0; rep < 5; ++rep) {
        std::vector<u32> again;
        set.aoi_query(math::Vec3{10.f, -20.f, 5.f}, 150.f, again);
        CHECK(again == first);  // byte-identical id-sorted result every call
    }
    // ...and matches the oracle.
    CHECK(first == brute_sorted(math::Vec3{10.f, -20.f, 5.f}, 150.f, all));
}

TEST_CASE("net: InterestSet scales to 2000 entities and matches the oracle",
          "[net][interest][aoi][broadphase][scale]") {
    std::mt19937 rng(0x5CA1Eu);
    std::uniform_real_distribution<f32> coord(-1000.f, 1000.f);
    std::vector<EntityPos> all;
    all.reserve(2000);
    for (usize i = 0; i < 2000; ++i)
        all.push_back(EntityPos{static_cast<u32>(i + 1),
                                math::Vec3{coord(rng), coord(rng), coord(rng)}});
    InterestSet set;
    set.rebuild(all);
    REQUIRE(set.size() == 2000);

    for (u32 q = 0; q < 8; ++q) {
        const math::Vec3 center{coord(rng), coord(rng), coord(rng)};
        const f32 radius = 50.f + static_cast<f32>(rng() % 900);
        std::vector<u32> got;
        set.aoi_query(center, radius, got);
        CHECK(got == brute_sorted(center, radius, all));
    }
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
