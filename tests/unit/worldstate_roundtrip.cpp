// SPDX-License-Identifier: MIT
// Psynder-GX — engine/worldstate round-trip + interpolation tests.
//
// Covers the contract that lane 18 (lag-comp) depends on:
//   * Header magic / version / entity_count survive a write→read round-trip.
//   * Single-entity lerp at t=0 == a, t=1 == b.
//   * Two-entity lerp at t=0.5 is the exact mid-point in every lerped field.
//   * Records in a / b in a different order are still matched correctly.
//   * Mismatched entity-id between a and b is rejected.
//   * Wrong-size buffers are rejected by both writer (assert) and reader.
//   * `lerp_for_lagcomp` adapter forwards correctly into interpolate_world_state.

#include <catch2/catch_test_macros.hpp>

#include "worldstate/LagCompAdapter.h"
#include "worldstate/WorldState.h"

#include <array>
#include <cstring>
#include <vector>

using namespace psynder;
using namespace psynder::worldstate;

namespace {

// Helper — build a deterministic EntityRecord from a small seed so each test
// can construct a few distinct records without repeating the field list.
EntityRecord make_record(u32 id, u32 archetype, f32 base, u32 flags = 0) {
    EntityRecord r{};
    r.entity_id      = id;
    r.archetype_hash = archetype;
    r.pos_x = base + 0.0f;
    r.pos_y = base + 1.0f;
    r.pos_z = base + 2.0f;
    r.yaw_y = base + 3.0f;
    r.vel_x = base + 4.0f;
    r.vel_y = base + 5.0f;
    r.vel_z = base + 6.0f;
    r.flags = flags;
    return r;
}

}  // namespace

TEST_CASE("worldstate: layout constants are wire-frozen", "[worldstate]") {
    // These are part of the wire contract; lane 18 reads buffers by stride.
    STATIC_REQUIRE(sizeof(WorldStateHeader) == 16);
    STATIC_REQUIRE(sizeof(EntityRecord)     == 40);
    STATIC_REQUIRE(alignof(EntityRecord)    ==  4);
    STATIC_REQUIRE(kWorldStateMagic         == 0x57533031u);  // 'W''S''0''1'
    STATIC_REQUIRE(kWorldStateVersion       == 1);
}

TEST_CASE("worldstate: header round-trips magic + version + count", "[worldstate]") {
    std::array<EntityRecord, 3> in{
        make_record(10u, 0xABCDu, 1.0f),
        make_record(11u, 0xABCDu, 2.0f),
        make_record(12u, 0x1234u, 3.0f),
    };

    const usize buf_sz = compute_world_state_bytes(in.size());
    REQUIRE(buf_sz == sizeof(WorldStateHeader) + 3 * sizeof(EntityRecord));

    std::vector<u8> buf(buf_sz);
    write_world_state(std::span<const EntityRecord>(in.data(), in.size()),
                      std::span<u8>(buf.data(), buf.size()));

    u16 version = 0, count = 0;
    REQUIRE(read_world_state_header(
        std::span<const u8>(buf.data(), buf.size()), &version, &count));
    CHECK(version == kWorldStateVersion);
    CHECK(count   == 3);

    // Records survive the round-trip byte-for-byte.
    const auto* recs = reinterpret_cast<const EntityRecord*>(
        buf.data() + sizeof(WorldStateHeader));
    for (usize i = 0; i < in.size(); ++i) {
        CHECK(recs[i].entity_id      == in[i].entity_id);
        CHECK(recs[i].archetype_hash == in[i].archetype_hash);
        CHECK(recs[i].pos_x          == in[i].pos_x);
        CHECK(recs[i].yaw_y          == in[i].yaw_y);
        CHECK(recs[i].flags          == in[i].flags);
    }
}

TEST_CASE("worldstate: read_world_state_header rejects bad inputs", "[worldstate]") {
    SECTION("buffer too small for header") {
        std::array<u8, 4> tiny{};
        u16 v = 0xFFFF, c = 0xFFFF;
        REQUIRE_FALSE(read_world_state_header(
            std::span<const u8>(tiny.data(), tiny.size()), &v, &c));
        // Outputs left unchanged.
        CHECK(v == 0xFFFF);
        CHECK(c == 0xFFFF);
    }
    SECTION("wrong magic") {
        std::vector<u8> buf(compute_world_state_bytes(0), 0);
        // All-zero header => magic 0.
        u16 v = 0, c = 0;
        REQUIRE_FALSE(read_world_state_header(
            std::span<const u8>(buf.data(), buf.size()), &v, &c));
    }
    SECTION("declared count overruns buffer") {
        // Header says 5 entities but only enough bytes for 0.
        WorldStateHeader hdr{};
        hdr.magic        = kWorldStateMagic;
        hdr.version      = kWorldStateVersion;
        hdr.entity_count = 5;
        std::vector<u8> buf(sizeof(WorldStateHeader));
        std::memcpy(buf.data(), &hdr, sizeof(hdr));
        u16 v = 0, c = 0;
        REQUIRE_FALSE(read_world_state_header(
            std::span<const u8>(buf.data(), buf.size()), &v, &c));
    }
}

TEST_CASE("worldstate: single-entity lerp at t=0 reproduces a", "[worldstate]") {
    std::array<EntityRecord, 1> a_in{ make_record(1u, 0x1u, 100.0f, 0xAA) };
    std::array<EntityRecord, 1> b_in{ make_record(1u, 0x1u, 200.0f, 0xBB) };

    const usize sz = compute_world_state_bytes(1);
    std::vector<u8> a_buf(sz), b_buf(sz), out_buf(sz);
    write_world_state(std::span<const EntityRecord>(a_in), std::span<u8>(a_buf));
    write_world_state(std::span<const EntityRecord>(b_in), std::span<u8>(b_buf));

    REQUIRE(interpolate_world_state(
        std::span<const u8>(a_buf), std::span<const u8>(b_buf),
        0.0f, std::span<u8>(out_buf)));

    const auto* out_recs = reinterpret_cast<const EntityRecord*>(
        out_buf.data() + sizeof(WorldStateHeader));
    CHECK(out_recs[0].entity_id      == a_in[0].entity_id);
    CHECK(out_recs[0].archetype_hash == a_in[0].archetype_hash);
    CHECK(out_recs[0].pos_x          == a_in[0].pos_x);
    CHECK(out_recs[0].pos_y          == a_in[0].pos_y);
    CHECK(out_recs[0].pos_z          == a_in[0].pos_z);
    CHECK(out_recs[0].yaw_y          == a_in[0].yaw_y);
    CHECK(out_recs[0].vel_x          == a_in[0].vel_x);
    CHECK(out_recs[0].vel_y          == a_in[0].vel_y);
    CHECK(out_recs[0].vel_z          == a_in[0].vel_z);
    // flags snaps to a at t<0.5.
    CHECK(out_recs[0].flags          == 0xAAu);
}

TEST_CASE("worldstate: single-entity lerp at t=1 reproduces b", "[worldstate]") {
    std::array<EntityRecord, 1> a_in{ make_record(1u, 0x1u, 100.0f, 0xAA) };
    std::array<EntityRecord, 1> b_in{ make_record(1u, 0x1u, 200.0f, 0xBB) };

    const usize sz = compute_world_state_bytes(1);
    std::vector<u8> a_buf(sz), b_buf(sz), out_buf(sz);
    write_world_state(std::span<const EntityRecord>(a_in), std::span<u8>(a_buf));
    write_world_state(std::span<const EntityRecord>(b_in), std::span<u8>(b_buf));

    REQUIRE(interpolate_world_state(
        std::span<const u8>(a_buf), std::span<const u8>(b_buf),
        1.0f, std::span<u8>(out_buf)));

    const auto* out_recs = reinterpret_cast<const EntityRecord*>(
        out_buf.data() + sizeof(WorldStateHeader));
    CHECK(out_recs[0].entity_id      == b_in[0].entity_id);
    CHECK(out_recs[0].pos_x          == b_in[0].pos_x);
    CHECK(out_recs[0].pos_y          == b_in[0].pos_y);
    CHECK(out_recs[0].pos_z          == b_in[0].pos_z);
    CHECK(out_recs[0].yaw_y          == b_in[0].yaw_y);
    CHECK(out_recs[0].vel_x          == b_in[0].vel_x);
    CHECK(out_recs[0].vel_y          == b_in[0].vel_y);
    CHECK(out_recs[0].vel_z          == b_in[0].vel_z);
    // flags snaps to b at t>=0.5.
    CHECK(out_recs[0].flags          == 0xBBu);
}

TEST_CASE("worldstate: two-entity lerp at t=0.5 is exact midpoint", "[worldstate]") {
    std::array<EntityRecord, 2> a_in{
        make_record(7u,  0x11u,  0.0f),
        make_record(13u, 0x22u, 10.0f),
    };
    std::array<EntityRecord, 2> b_in{
        make_record(7u,  0x11u, 100.0f),
        make_record(13u, 0x22u, 200.0f),
    };

    const usize sz = compute_world_state_bytes(2);
    std::vector<u8> a_buf(sz), b_buf(sz), out_buf(sz);
    write_world_state(std::span<const EntityRecord>(a_in), std::span<u8>(a_buf));
    write_world_state(std::span<const EntityRecord>(b_in), std::span<u8>(b_buf));

    REQUIRE(interpolate_world_state(
        std::span<const u8>(a_buf), std::span<const u8>(b_buf),
        0.5f, std::span<u8>(out_buf)));

    const auto* out_recs = reinterpret_cast<const EntityRecord*>(
        out_buf.data() + sizeof(WorldStateHeader));

    // a + (b - a)*0.5 == midpoint of each field for both entities.
    for (usize i = 0; i < 2; ++i) {
        CHECK(out_recs[i].entity_id == a_in[i].entity_id);
        CHECK(out_recs[i].pos_x == (a_in[i].pos_x + (b_in[i].pos_x - a_in[i].pos_x) * 0.5f));
        CHECK(out_recs[i].pos_y == (a_in[i].pos_y + (b_in[i].pos_y - a_in[i].pos_y) * 0.5f));
        CHECK(out_recs[i].pos_z == (a_in[i].pos_z + (b_in[i].pos_z - a_in[i].pos_z) * 0.5f));
        CHECK(out_recs[i].yaw_y == (a_in[i].yaw_y + (b_in[i].yaw_y - a_in[i].yaw_y) * 0.5f));
        CHECK(out_recs[i].vel_x == (a_in[i].vel_x + (b_in[i].vel_x - a_in[i].vel_x) * 0.5f));
        CHECK(out_recs[i].vel_y == (a_in[i].vel_y + (b_in[i].vel_y - a_in[i].vel_y) * 0.5f));
        CHECK(out_recs[i].vel_z == (a_in[i].vel_z + (b_in[i].vel_z - a_in[i].vel_z) * 0.5f));
    }
}

TEST_CASE("worldstate: out-of-order records still match by id", "[worldstate]") {
    // Same entities, opposite order in `b`. lerp must match by id, not by index.
    std::array<EntityRecord, 2> a_in{
        make_record(7u,  0x11u,  0.0f),
        make_record(13u, 0x22u, 10.0f),
    };
    std::array<EntityRecord, 2> b_in{
        make_record(13u, 0x22u, 200.0f),  // <-- reversed
        make_record(7u,  0x11u, 100.0f),
    };

    const usize sz = compute_world_state_bytes(2);
    std::vector<u8> a_buf(sz), b_buf(sz), out_buf(sz);
    write_world_state(std::span<const EntityRecord>(a_in), std::span<u8>(a_buf));
    write_world_state(std::span<const EntityRecord>(b_in), std::span<u8>(b_buf));

    REQUIRE(interpolate_world_state(
        std::span<const u8>(a_buf), std::span<const u8>(b_buf),
        0.5f, std::span<u8>(out_buf)));

    const auto* out_recs = reinterpret_cast<const EntityRecord*>(
        out_buf.data() + sizeof(WorldStateHeader));
    // out preserves the order of `a`.
    CHECK(out_recs[0].entity_id == 7u);
    CHECK(out_recs[1].entity_id == 13u);
    // And id=7 was matched to b's id=7 (base 100), not to b[0] which is id=13.
    // pos_x for id=7: a=0, b=100, t=0.5 → 50.
    CHECK(out_recs[0].pos_x == 50.0f);
    // pos_x for id=13: a=10, b=200, t=0.5 → 105.
    CHECK(out_recs[1].pos_x == 105.0f);
}

TEST_CASE("worldstate: mismatched entity id is rejected", "[worldstate]") {
    std::array<EntityRecord, 1> a_in{ make_record(1u, 0x1u, 0.0f) };
    std::array<EntityRecord, 1> b_in{ make_record(2u, 0x1u, 0.0f) };  // different id

    const usize sz = compute_world_state_bytes(1);
    std::vector<u8> a_buf(sz), b_buf(sz), out_buf(sz);
    write_world_state(std::span<const EntityRecord>(a_in), std::span<u8>(a_buf));
    write_world_state(std::span<const EntityRecord>(b_in), std::span<u8>(b_buf));

    REQUIRE_FALSE(interpolate_world_state(
        std::span<const u8>(a_buf), std::span<const u8>(b_buf),
        0.5f, std::span<u8>(out_buf)));
}

TEST_CASE("worldstate: mismatched archetype is rejected", "[worldstate]") {
    // Same id, archetype mid-stream change — could be ECS corruption.
    std::array<EntityRecord, 1> a_in{ make_record(1u, 0x1u, 0.0f) };
    std::array<EntityRecord, 1> b_in{ make_record(1u, 0x2u, 0.0f) };

    const usize sz = compute_world_state_bytes(1);
    std::vector<u8> a_buf(sz), b_buf(sz), out_buf(sz);
    write_world_state(std::span<const EntityRecord>(a_in), std::span<u8>(a_buf));
    write_world_state(std::span<const EntityRecord>(b_in), std::span<u8>(b_buf));

    REQUIRE_FALSE(interpolate_world_state(
        std::span<const u8>(a_buf), std::span<const u8>(b_buf),
        0.5f, std::span<u8>(out_buf)));
}

TEST_CASE("worldstate: wrong-size output buffer is rejected", "[worldstate]") {
    std::array<EntityRecord, 1> a_in{ make_record(1u, 0x1u, 0.0f) };
    std::array<EntityRecord, 1> b_in{ make_record(1u, 0x1u, 0.0f) };

    const usize sz = compute_world_state_bytes(1);
    std::vector<u8> a_buf(sz), b_buf(sz);
    write_world_state(std::span<const EntityRecord>(a_in), std::span<u8>(a_buf));
    write_world_state(std::span<const EntityRecord>(b_in), std::span<u8>(b_buf));

    // out too short.
    std::vector<u8> short_out(sz - 1);
    REQUIRE_FALSE(interpolate_world_state(
        std::span<const u8>(a_buf), std::span<const u8>(b_buf),
        0.5f, std::span<u8>(short_out)));

    // out too long.
    std::vector<u8> long_out(sz + 1);
    REQUIRE_FALSE(interpolate_world_state(
        std::span<const u8>(a_buf), std::span<const u8>(b_buf),
        0.5f, std::span<u8>(long_out)));
}

TEST_CASE("worldstate: differing entity_count is rejected", "[worldstate]") {
    std::array<EntityRecord, 1> a_in{ make_record(1u, 0x1u, 0.0f) };
    std::array<EntityRecord, 2> b_in{ make_record(1u, 0x1u, 0.0f),
                                      make_record(2u, 0x1u, 0.0f) };

    std::vector<u8> a_buf(compute_world_state_bytes(1));
    std::vector<u8> b_buf(compute_world_state_bytes(2));
    write_world_state(std::span<const EntityRecord>(a_in), std::span<u8>(a_buf));
    write_world_state(std::span<const EntityRecord>(b_in), std::span<u8>(b_buf));

    std::vector<u8> out_buf(compute_world_state_bytes(1));
    REQUIRE_FALSE(interpolate_world_state(
        std::span<const u8>(a_buf), std::span<const u8>(b_buf),
        0.5f, std::span<u8>(out_buf)));
}

TEST_CASE("worldstate: empty snapshot lerps to empty snapshot", "[worldstate]") {
    // Server just spawned, 0 entities. The lerp is a no-op but must succeed.
    const usize sz = compute_world_state_bytes(0);
    std::vector<u8> a_buf(sz), b_buf(sz), out_buf(sz);
    write_world_state(std::span<const EntityRecord>{}, std::span<u8>(a_buf));
    write_world_state(std::span<const EntityRecord>{}, std::span<u8>(b_buf));

    REQUIRE(interpolate_world_state(
        std::span<const u8>(a_buf), std::span<const u8>(b_buf),
        0.5f, std::span<u8>(out_buf)));

    u16 v = 0, c = 0xFFFF;
    REQUIRE(read_world_state_header(
        std::span<const u8>(out_buf), &v, &c));
    CHECK(v == kWorldStateVersion);
    CHECK(c == 0);
}

TEST_CASE("worldstate: parallel path produces same result as serial", "[worldstate]") {
    // Above kParallelEntityThreshold we switch to JobSystem::parallel_for; the
    // result must be byte-identical to the serial path (lockstep determinism).
    constexpr usize kN = kParallelEntityThreshold * 2 + 7;  // 135
    std::vector<EntityRecord> a_in, b_in;
    a_in.reserve(kN);
    b_in.reserve(kN);
    for (u32 i = 0; i < kN; ++i) {
        a_in.push_back(make_record(i + 1, 0x42u, static_cast<f32>(i)));
        b_in.push_back(make_record(i + 1, 0x42u, static_cast<f32>(i) + 1000.0f));
    }

    const usize sz = compute_world_state_bytes(kN);
    std::vector<u8> a_buf(sz), b_buf(sz), out_buf(sz);
    write_world_state(std::span<const EntityRecord>(a_in.data(), a_in.size()),
                      std::span<u8>(a_buf));
    write_world_state(std::span<const EntityRecord>(b_in.data(), b_in.size()),
                      std::span<u8>(b_buf));

    REQUIRE(interpolate_world_state(
        std::span<const u8>(a_buf), std::span<const u8>(b_buf),
        0.25f, std::span<u8>(out_buf)));

    const auto* out_recs = reinterpret_cast<const EntityRecord*>(
        out_buf.data() + sizeof(WorldStateHeader));
    // Expected: a.pos_x + 1000.0f * 0.25 == a.pos_x + 250.
    for (u32 i = 0; i < kN; ++i) {
        CHECK(out_recs[i].entity_id == i + 1);
        CHECK(out_recs[i].pos_x == static_cast<f32>(i) + 250.0f);
    }
}

TEST_CASE("worldstate: lerp_for_lagcomp adapter forwards correctly", "[worldstate]") {
    std::array<EntityRecord, 1> a_in{ make_record(99u, 0x77u,  0.0f) };
    std::array<EntityRecord, 1> b_in{ make_record(99u, 0x77u, 80.0f) };

    const usize sz = compute_world_state_bytes(1);
    std::vector<u8> a_buf(sz), b_buf(sz), out_buf(sz);
    write_world_state(std::span<const EntityRecord>(a_in), std::span<u8>(a_buf));
    write_world_state(std::span<const EntityRecord>(b_in), std::span<u8>(b_buf));

    // Happy path through the void* signature.
    REQUIRE(lerp_for_lagcomp(
        a_buf.data(), a_buf.size(),
        b_buf.data(), b_buf.size(),
        0.5f,
        out_buf.data(), out_buf.size()));
    const auto* out_recs = reinterpret_cast<const EntityRecord*>(
        out_buf.data() + sizeof(WorldStateHeader));
    CHECK(out_recs[0].entity_id == 99u);
    // pos_x: a=0, b=80, t=0.5 → 40.
    CHECK(out_recs[0].pos_x == 40.0f);

    // Nulls rejected.
    REQUIRE_FALSE(lerp_for_lagcomp(
        nullptr, a_buf.size(), b_buf.data(), b_buf.size(),
        0.5f, out_buf.data(), out_buf.size()));
    REQUIRE_FALSE(lerp_for_lagcomp(
        a_buf.data(), a_buf.size(), nullptr, b_buf.size(),
        0.5f, out_buf.data(), out_buf.size()));
    REQUIRE_FALSE(lerp_for_lagcomp(
        a_buf.data(), a_buf.size(), b_buf.data(), b_buf.size(),
        0.5f, nullptr, out_buf.size()));

    // Mismatched sizes rejected.
    REQUIRE_FALSE(lerp_for_lagcomp(
        a_buf.data(), a_buf.size(), b_buf.data(), b_buf.size() - 1,
        0.5f, out_buf.data(), out_buf.size()));
}

TEST_CASE("worldstate: out-of-range t clamps to [0,1]", "[worldstate]") {
    std::array<EntityRecord, 1> a_in{ make_record(1u, 0x1u, 0.0f) };
    std::array<EntityRecord, 1> b_in{ make_record(1u, 0x1u, 100.0f) };

    const usize sz = compute_world_state_bytes(1);
    std::vector<u8> a_buf(sz), b_buf(sz), out_buf(sz);
    write_world_state(std::span<const EntityRecord>(a_in), std::span<u8>(a_buf));
    write_world_state(std::span<const EntityRecord>(b_in), std::span<u8>(b_buf));

    REQUIRE(interpolate_world_state(
        std::span<const u8>(a_buf), std::span<const u8>(b_buf),
        -0.5f, std::span<u8>(out_buf)));
    const auto* out_recs = reinterpret_cast<const EntityRecord*>(
        out_buf.data() + sizeof(WorldStateHeader));
    // Clamped to t=0 → reproduces a.
    CHECK(out_recs[0].pos_x == 0.0f);

    REQUIRE(interpolate_world_state(
        std::span<const u8>(a_buf), std::span<const u8>(b_buf),
        2.0f, std::span<u8>(out_buf)));
    const auto* out_recs2 = reinterpret_cast<const EntityRecord*>(
        out_buf.data() + sizeof(WorldStateHeader));
    // Clamped to t=1 → reproduces b.
    CHECK(out_recs2[0].pos_x == 100.0f);
}
