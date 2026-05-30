// SPDX-License-Identifier: MIT OR Apache-2.0
// Psynder-GX — lane 18 / net BIT-LEVEL quantized snapshot DELTA codec tests.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/Types.h"
#include "net/DeltaBitCodec.h"
#include "net/SnapshotPackDelta.h"
#include "net/SnapshotReplication.h"

using namespace psynder;
using namespace psynder::net;

namespace {

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

TEST_CASE("net-bit-delta: a no-change delta is tiny and round-trips the set",
          "[net]") {
    const f32 res = 0.001f;
    const std::vector<EntityState> prev = baseline_states();
    const std::vector<EntityState> curr = prev;  // identical

    std::vector<u8> buf;
    encode_bit_delta(prev, curr, res, buf);

    // Nothing changed: two 32-bit counts (both zero) is all that's on the wire =
    // 64 bits = 8 bytes. Tiny.
    CHECK(buf.size() <= 8u);

    std::vector<EntityState> out;
    REQUIRE(decode_bit_delta(prev, buf, res, out));

    REQUIRE(out.size() == prev.size());
    for (const EntityState& p : prev) {
        const EntityState* got = find_by_id(out, p.id);
        REQUIRE(got != nullptr);
        CHECK(got->pos[0] == Catch::Approx(p.pos[0]).margin(res / 2.0));
        CHECK(got->pos[1] == Catch::Approx(p.pos[1]).margin(res / 2.0));
        CHECK(got->pos[2] == Catch::Approx(p.pos[2]).margin(res / 2.0));
        CHECK(got->yaw_deg == Catch::Approx(p.yaw_deg).margin(0.001));
    }
}

TEST_CASE("net-bit-delta: a small move round-trips and packs smaller than the "
          "byte-form delta",
          "[net]") {
    const f32 res = 0.001f;  // 1 mm steps
    const std::vector<EntityState> prev = baseline_states();
    std::vector<EntityState> curr = prev;

    // Nudge id 7 by a few centimetres — far above the 1 mm step (so it IS a
    // change) but a tiny per-field delta of only tens of steps that zigzags into
    // a handful of bits.
    for (EntityState& s : curr) {
        if (s.id == 7) {
            s.pos[0] += 0.05f;   // +50 mm  -> 50 steps
            s.pos[1] -= 0.03f;   // -30 mm  -> 30 steps
            s.pos[2] += 0.012f;  // +12 mm  -> 12 steps
            s.yaw_deg += 0.25f;  // +250 milli-deg
        }
    }

    std::vector<u8> bit_buf;
    encode_bit_delta(prev, curr, res, bit_buf);

    // The bit form is small for one small move.
    CHECK(bit_buf.size() < 20u);

    // ...and strictly smaller than the equivalent byte-granular SnapshotPackDelta
    // for the same delta (which spends a fixed 20-byte record on the moved id).
    std::vector<u8> byte_buf;
    pack_quantized_delta(prev, curr, res, byte_buf);
    CHECK(bit_buf.size() < byte_buf.size());

    std::vector<EntityState> out;
    REQUIRE(decode_bit_delta(prev, bit_buf, res, out));

    REQUIRE(out.size() == prev.size());
    const EntityState* moved = find_by_id(out, 7);
    REQUIRE(moved != nullptr);
    CHECK(moved->pos[0] == Catch::Approx(0.05f).margin(res / 2.0));
    CHECK(moved->pos[1] == Catch::Approx(-0.03f).margin(res / 2.0));
    CHECK(moved->pos[2] == Catch::Approx(0.012f).margin(res / 2.0));
    CHECK(moved->yaw_deg == Catch::Approx(0.25f).margin(0.001));
}

TEST_CASE("net-bit-delta: an add round-trips and appears in the output",
          "[net]") {
    const f32 res = 0.001f;
    const std::vector<EntityState> prev = baseline_states();
    std::vector<EntityState> curr = prev;
    curr.push_back(make_state(9999, 1000.5f, -1000.5f, -0.5f, 359.999f));

    std::vector<u8> buf;
    encode_bit_delta(prev, curr, res, buf);

    std::vector<EntityState> out;
    REQUIRE(decode_bit_delta(prev, buf, res, out));

    REQUIRE(out.size() == curr.size());
    const EntityState* added = find_by_id(out, 9999);
    REQUIRE(added != nullptr);
    CHECK(added->pos[0] == Catch::Approx(1000.5f).margin(res / 2.0));
    CHECK(added->pos[1] == Catch::Approx(-1000.5f).margin(res / 2.0));
    CHECK(added->pos[2] == Catch::Approx(-0.5f).margin(res / 2.0));
    CHECK(added->yaw_deg == Catch::Approx(359.999f).margin(0.001));
}

TEST_CASE("net-bit-delta: a remove round-trips and drops the id", "[net]") {
    const f32 res = 0.001f;
    const std::vector<EntityState> prev = baseline_states();
    std::vector<EntityState> curr;
    for (const EntityState& s : prev) {
        if (s.id != 42) curr.push_back(s);  // drop id 42, keep the rest
    }

    std::vector<u8> buf;
    encode_bit_delta(prev, curr, res, buf);

    std::vector<EntityState> out;
    REQUIRE(decode_bit_delta(prev, buf, res, out));

    REQUIRE(out.size() == curr.size());
    CHECK(find_by_id(out, 42) == nullptr);  // gone
    CHECK(find_by_id(out, 1) != nullptr);
    CHECK(find_by_id(out, 7) != nullptr);
    CHECK(find_by_id(out, 256) != nullptr);
}

TEST_CASE("net-bit-delta: a large jump still round-trips", "[net]") {
    const f32 res = 0.001f;
    const std::vector<EntityState> prev = baseline_states();
    std::vector<EntityState> curr = prev;

    // A big teleport: thousands of metres and a yaw swing — the per-field zigzag
    // is large, the width header expands, but it must round-trip exactly.
    for (EntityState& s : curr) {
        if (s.id == 1) {
            s.pos[0] = 5000.0f;
            s.pos[1] = -8000.0f;
            s.pos[2] = 1234.567f;
            s.yaw_deg = 179.999f;
        }
    }

    std::vector<u8> buf;
    encode_bit_delta(prev, curr, res, buf);

    std::vector<EntityState> out;
    REQUIRE(decode_bit_delta(prev, buf, res, out));

    const EntityState* jumped = find_by_id(out, 1);
    REQUIRE(jumped != nullptr);
    CHECK(jumped->pos[0] == Catch::Approx(5000.0f).margin(res / 2.0));
    CHECK(jumped->pos[1] == Catch::Approx(-8000.0f).margin(res / 2.0));
    CHECK(jumped->pos[2] == Catch::Approx(1234.567f).margin(res / 2.0));
    CHECK(jumped->yaw_deg == Catch::Approx(179.999f).margin(0.001));
}

TEST_CASE("net-bit-delta: an add, a remove and a move all round-trip together",
          "[net]") {
    const f32 res = 0.001f;
    const std::vector<EntityState> prev = baseline_states();
    std::vector<EntityState> curr = prev;
    curr.push_back(make_state(9999, 1.0f, 2.0f, 3.0f, 12.0f));  // add
    curr.erase(std::remove_if(curr.begin(), curr.end(),
                              [](const EntityState& s) { return s.id == 42; }),
               curr.end());                                     // remove
    for (EntityState& s : curr) {
        if (s.id == 7) { s.pos[0] = 5.0f; s.yaw_deg = 90.0f; }  // move
    }

    std::vector<u8> buf;
    encode_bit_delta(prev, curr, res, buf);

    std::vector<EntityState> out;
    REQUIRE(decode_bit_delta(prev, buf, res, out));

    REQUIRE(out.size() == curr.size());
    CHECK(find_by_id(out, 42) == nullptr);
    const EntityState* added = find_by_id(out, 9999);
    REQUIRE(added != nullptr);
    CHECK(added->pos[0] == Catch::Approx(1.0f).margin(res / 2.0));
    const EntityState* moved = find_by_id(out, 7);
    REQUIRE(moved != nullptr);
    CHECK(moved->pos[0] == Catch::Approx(5.0f).margin(res / 2.0));
    CHECK(moved->yaw_deg == Catch::Approx(90.0f).margin(0.001));
}

TEST_CASE("net-bit-delta: output ids are ascending regardless of input order",
          "[net]") {
    const f32 res = 0.001f;

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
    encode_bit_delta(prev, curr, res, buf);

    std::vector<EntityState> out;
    REQUIRE(decode_bit_delta(prev, buf, res, out));

    REQUIRE(out.size() == 4u);
    for (usize i = 1; i < out.size(); ++i) {
        CHECK(out[i - 1].id < out[i].id);  // strictly ascending
    }
}

TEST_CASE("net-bit-delta: truncated bytes make decode return false without "
          "touching out",
          "[net]") {
    const f32 res = 0.001f;
    const std::vector<EntityState> prev = baseline_states();
    std::vector<EntityState> curr = prev;
    curr.push_back(make_state(9999, 1.0f, 2.0f, 3.0f, 12.0f));  // one change

    std::vector<u8> buf;
    encode_bit_delta(prev, curr, res, buf);
    REQUIRE(buf.size() > 2u);

    // A sentinel `out` that must survive every failed decode untouched.
    std::vector<EntityState> out;
    out.push_back(make_state(123, 1.f, 1.f, 1.f, 1.f));
    const std::vector<EntityState> sentinel = out;

    // Lop off the tail at several truncation points; every one must fail and
    // leave the sentinel intact.
    for (const usize keep : {usize{1}, usize{4}, usize{8}, buf.size() - 1}) {
        std::vector<u8> truncated(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(keep));
        CHECK_FALSE(decode_bit_delta(prev, truncated, res, out));
        REQUIRE(out.size() == sentinel.size());
        CHECK(out[0].id == sentinel[0].id);
    }
}

TEST_CASE("net-bit-delta: two identical encodes are byte-identical",
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
    encode_bit_delta(prev, curr, res, a);
    encode_bit_delta(prev, curr, res, b);

    REQUIRE(a.size() == b.size());
    CHECK(std::memcmp(a.data(), b.data(), a.size()) == 0);
}
