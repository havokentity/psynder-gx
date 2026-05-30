// SPDX-License-Identifier: MIT OR Apache-2.0
// Psynder-GX — lane 18 / net quantized snapshot DELTA codec tests.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/Types.h"
#include "net/SnapshotPackDelta.h"
#include "net/SnapshotReplication.h"

using namespace psynder;
using namespace psynder::net;

namespace {

// Read a little-endian u32 from a byte buffer at `off` (test-side mirror of the
// codec's LE convention, used to assert raw header values directly).
u32 peek_u32_le(const std::vector<u8>& b, usize off) {
    return static_cast<u32>(b[off]) |
           (static_cast<u32>(b[off + 1]) << 8) |
           (static_cast<u32>(b[off + 2]) << 16) |
           (static_cast<u32>(b[off + 3]) << 24);
}

// Find a reconstructed state by id; nullptr if absent.
const EntityState* find_by_id(const std::vector<EntityState>& v, u32 id) {
    for (const EntityState& s : v) {
        if (s.id == id) return &s;
    }
    return nullptr;
}

EntityState make_state(u32 id, f32 x, f32 y, f32 z, f32 yaw) {
    EntityState s{};
    s.id = id;
    s.pos[0] = x;
    s.pos[1] = y;
    s.pos[2] = z;
    s.yaw_deg = yaw;
    return s;
}

// A baseline spread of ids / positions / yaws, including negatives.
std::vector<EntityState> baseline_states() {
    std::vector<EntityState> v;
    v.push_back(make_state(1,    12.345f,  -7.89f,   0.001f, 271.5f));
    v.push_back(make_state(42,  -100.25f,   0.0f,   33.333f, -45.125f));
    v.push_back(make_state(7,     0.0f,     0.0f,    0.0f,    0.0f));
    v.push_back(make_state(256,  -3.14159f, 2.71828f,-0.123f,-180.0f));
    return v;
}

}  // namespace

TEST_CASE("net-pack-delta: no-change delta packs two zero counts and round-trips",
          "[net]") {
    const f32 res = 0.001f;
    const std::vector<EntityState> prev = baseline_states();
    const std::vector<EntityState> curr = prev;  // identical

    std::vector<u8> buf;
    pack_quantized_delta(prev, curr, res, buf);

    // Just the two u32 counts, both zero: removed_count, changed_count.
    REQUIRE(buf.size() == delta_size(0, 0));
    REQUIRE(buf.size() == 8u);
    CHECK(peek_u32_le(buf, 0) == 0u);  // removed_count
    CHECK(peek_u32_le(buf, 4) == 0u);  // changed_count

    std::vector<EntityState> out;
    REQUIRE(apply_quantized_delta(prev, buf, res, out));

    // Same set reconstructed (ascending id order: 1, 7, 42, 256).
    REQUIRE(out.size() == prev.size());
    for (const EntityState& p : prev) {
        const EntityState* got = find_by_id(out, p.id);
        REQUIRE(got != nullptr);
        CHECK(std::fabs(got->pos[0] - p.pos[0]) <= res / 2.f);
        CHECK(std::fabs(got->pos[1] - p.pos[1]) <= res / 2.f);
        CHECK(std::fabs(got->pos[2] - p.pos[2]) <= res / 2.f);
        CHECK(got->yaw_deg == Catch::Approx(p.yaw_deg).margin(0.001));
    }
}

TEST_CASE("net-pack-delta: a pure add appears in changed and round-trips",
          "[net]") {
    const f32 res = 0.001f;
    const std::vector<EntityState> prev = baseline_states();
    std::vector<EntityState> curr = prev;
    curr.push_back(make_state(9999, 1000.5f, -1000.5f, -0.5f, 359.999f));

    std::vector<u8> buf;
    pack_quantized_delta(prev, curr, res, buf);

    // No removals, exactly one changed (the new id).
    CHECK(peek_u32_le(buf, 0) == 0u);  // removed_count
    REQUIRE(buf.size() == delta_size(0, 1));
    CHECK(peek_u32_le(buf, 4) == 1u);  // changed_count

    std::vector<EntityState> out;
    REQUIRE(apply_quantized_delta(prev, buf, res, out));

    REQUIRE(out.size() == curr.size());
    const EntityState* added = find_by_id(out, 9999);
    REQUIRE(added != nullptr);
    CHECK(std::fabs(added->pos[0] - 1000.5f) <= res / 2.f);
    CHECK(std::fabs(added->pos[1] - (-1000.5f)) <= res / 2.f);
    CHECK(std::fabs(added->pos[2] - (-0.5f)) <= res / 2.f);
    CHECK(added->yaw_deg == Catch::Approx(359.999f).margin(0.001));
}

TEST_CASE("net-pack-delta: a pure remove appears in removed and is absent after apply",
          "[net]") {
    const f32 res = 0.001f;
    const std::vector<EntityState> prev = baseline_states();
    std::vector<EntityState> curr;
    // Drop id 42; keep the rest unchanged.
    for (const EntityState& s : prev) {
        if (s.id != 42) curr.push_back(s);
    }

    std::vector<u8> buf;
    pack_quantized_delta(prev, curr, res, buf);

    // Exactly one removal, no changes.
    REQUIRE(peek_u32_le(buf, 0) == 1u);  // removed_count
    CHECK(peek_u32_le(buf, 4) == 42u);   // the removed id itself
    // After the single removed id (at off 8) comes the changed count.
    CHECK(peek_u32_le(buf, 8) == 0u);    // changed_count
    REQUIRE(buf.size() == delta_size(1, 0));

    std::vector<EntityState> out;
    REQUIRE(apply_quantized_delta(prev, buf, res, out));

    REQUIRE(out.size() == curr.size());
    CHECK(find_by_id(out, 42) == nullptr);  // gone
    CHECK(find_by_id(out, 1) != nullptr);
    CHECK(find_by_id(out, 7) != nullptr);
    CHECK(find_by_id(out, 256) != nullptr);
}

TEST_CASE("net-pack-delta: a moved entity re-appears as changed and round-trips",
          "[net]") {
    const f32 res = 0.001f;
    const std::vector<EntityState> prev = baseline_states();
    std::vector<EntityState> curr = prev;
    // Move id 7 well beyond the 1 mm resolution.
    for (EntityState& s : curr) {
        if (s.id == 7) {
            s.pos[0] = 5.0f;
            s.pos[1] = -2.5f;
            s.pos[2] = 0.25f;
            s.yaw_deg = 90.0f;
        }
    }

    std::vector<u8> buf;
    pack_quantized_delta(prev, curr, res, buf);

    CHECK(peek_u32_le(buf, 0) == 0u);  // removed_count
    REQUIRE(buf.size() == delta_size(0, 1));
    CHECK(peek_u32_le(buf, 4) == 1u);  // exactly the moved entity

    std::vector<EntityState> out;
    REQUIRE(apply_quantized_delta(prev, buf, res, out));

    REQUIRE(out.size() == prev.size());
    const EntityState* moved = find_by_id(out, 7);
    REQUIRE(moved != nullptr);
    CHECK(std::fabs(moved->pos[0] - 5.0f) <= res / 2.f);
    CHECK(std::fabs(moved->pos[1] - (-2.5f)) <= res / 2.f);
    CHECK(std::fabs(moved->pos[2] - 0.25f) <= res / 2.f);
    CHECK(moved->yaw_deg == Catch::Approx(90.0f).margin(0.001));
}

TEST_CASE("net-pack-delta: a sub-step move is omitted yet apply yields the baseline",
          "[net]") {
    const f32 res = 0.01f;  // 1 cm steps
    const std::vector<EntityState> prev = baseline_states();
    std::vector<EntityState> curr = prev;
    // Nudge id 1 by less than half a cm so it quantizes to the same record.
    for (EntityState& s : curr) {
        if (s.id == 1) {
            s.pos[0] += 0.002f;  // 2 mm < 5 mm half-step
            s.pos[2] -= 0.003f;  // 3 mm < 5 mm half-step
        }
    }

    std::vector<u8> buf;
    pack_quantized_delta(prev, curr, res, buf);

    // The nudge is below the quantization step — nothing changed on the wire.
    CHECK(peek_u32_le(buf, 0) == 0u);  // removed_count
    CHECK(peek_u32_le(buf, 4) == 0u);  // changed_count — id 1 omitted
    REQUIRE(buf.size() == delta_size(0, 0));

    std::vector<EntityState> out;
    REQUIRE(apply_quantized_delta(prev, buf, res, out));

    // id 1 still reconstructs to the BASELINE quantized value (not the nudge).
    const EntityState* got = find_by_id(out, 1);
    REQUIRE(got != nullptr);
    CHECK(std::fabs(got->pos[0] - prev[0].pos[0]) <= res / 2.f);
    CHECK(std::fabs(got->pos[2] - prev[0].pos[2]) <= res / 2.f);
}

TEST_CASE("net-pack-delta: output ids are ascending regardless of input order",
          "[net]") {
    const f32 res = 0.001f;

    // Same logical sets, shuffled input order on both sides.
    std::vector<EntityState> prev;
    prev.push_back(make_state(256, 1.0f, 0.0f, 0.0f, 0.0f));
    prev.push_back(make_state(1,   0.0f, 0.0f, 0.0f, 0.0f));
    prev.push_back(make_state(42,  2.0f, 0.0f, 0.0f, 0.0f));

    std::vector<EntityState> curr;
    curr.push_back(make_state(42,  9.0f, 0.0f, 0.0f, 0.0f));   // moved
    curr.push_back(make_state(7,   3.0f, 0.0f, 0.0f, 0.0f));   // added
    curr.push_back(make_state(256, 1.0f, 0.0f, 0.0f, 0.0f));   // unchanged
    curr.push_back(make_state(1,   0.0f, 0.0f, 0.0f, 0.0f));   // unchanged

    std::vector<u8> buf;
    pack_quantized_delta(prev, curr, res, buf);

    // Changed records (ids 7 and 42) must be emitted ascending: 7 before 42.
    // removed_count at off 0 is 0, so changed_count is at off 4, records at 8.
    REQUIRE(peek_u32_le(buf, 0) == 0u);
    REQUIRE(peek_u32_le(buf, 4) == 2u);
    const usize rec0 = 8;                 // first changed record id
    const usize rec1 = rec0 + 20;         // second changed record id
    CHECK(peek_u32_le(buf, rec0) == 7u);
    CHECK(peek_u32_le(buf, rec1) == 42u);

    std::vector<EntityState> out;
    REQUIRE(apply_quantized_delta(prev, buf, res, out));

    // The reconstructed set is emitted in ascending id order.
    REQUIRE(out.size() == 4u);
    for (usize i = 1; i < out.size(); ++i) {
        CHECK(out[i - 1].id < out[i].id);
    }
}

TEST_CASE("net-pack-delta: truncated bytes make apply return false without touching out",
          "[net]") {
    const f32 res = 0.001f;
    const std::vector<EntityState> prev = baseline_states();
    std::vector<EntityState> curr = prev;
    curr.push_back(make_state(9999, 1.0f, 2.0f, 3.0f, 12.0f));  // one change

    std::vector<u8> buf;
    pack_quantized_delta(prev, curr, res, buf);
    REQUIRE(buf.size() == delta_size(0, 1));

    // A sentinel `out` that must survive every failed apply untouched.
    std::vector<EntityState> out;
    out.push_back(make_state(123, 1.f, 1.f, 1.f, 1.f));
    const std::vector<EntityState> sentinel = out;

    // 3 bytes — not even the removed-count header.
    {
        std::vector<u8> too_short(buf.begin(), buf.begin() + 3);
        CHECK_FALSE(apply_quantized_delta(prev, too_short, res, out));
        REQUIRE(out.size() == sentinel.size());
        CHECK(out[0].id == sentinel[0].id);
    }

    // removed-count header present but the changed-count header is missing.
    {
        std::vector<u8> no_changed_hdr(buf.begin(), buf.begin() + 4);
        CHECK_FALSE(apply_quantized_delta(prev, no_changed_hdr, res, out));
        REQUIRE(out.size() == sentinel.size());
        CHECK(out[0].id == sentinel[0].id);
    }

    // Both headers present (claims 1 changed record) but the record is missing.
    {
        std::vector<u8> headers_only(buf.begin(), buf.begin() + 8);
        CHECK_FALSE(apply_quantized_delta(prev, headers_only, res, out));
        REQUIRE(out.size() == sentinel.size());
        CHECK(out[0].id == sentinel[0].id);
    }

    // One byte short of the complete final record.
    {
        std::vector<u8> one_short(buf.begin(), buf.end() - 1);
        CHECK_FALSE(apply_quantized_delta(prev, one_short, res, out));
        REQUIRE(out.size() == sentinel.size());
        CHECK(out[0].id == sentinel[0].id);
    }
}

TEST_CASE("net-pack-delta: two identical packs are byte-identical",
          "[net][determinism]") {
    const f32 res = 0.001f;
    const std::vector<EntityState> prev = baseline_states();
    std::vector<EntityState> curr = prev;
    curr.push_back(make_state(9999, 1.0f, 2.0f, 3.0f, 12.0f));  // add
    curr.erase(std::remove_if(curr.begin(), curr.end(),
                              [](const EntityState& s) { return s.id == 42; }),
               curr.end());                                     // remove
    for (EntityState& s : curr) {
        if (s.id == 7) s.pos[0] = 5.0f;                         // move
    }

    std::vector<u8> a;
    std::vector<u8> b;
    pack_quantized_delta(prev, curr, res, a);
    pack_quantized_delta(prev, curr, res, b);

    REQUIRE(a.size() == b.size());
    CHECK(std::memcmp(a.data(), b.data(), a.size()) == 0);
}
