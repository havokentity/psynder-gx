// SPDX-License-Identifier: MIT OR Apache-2.0
// Psynder-GX — lane 18 / net client-side snapshot interpolation buffer tests.

#include <cmath>
#include <cstring>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/Types.h"
#include "net/SnapshotInterp.h"
#include "net/SnapshotReplication.h"

using namespace psynder;
using namespace psynder::net;

namespace {

EntityState make_state(u32 id, f32 x, f32 y, f32 z, f32 yaw) {
    EntityState s{};
    s.id = id;
    s.pos[0] = x;
    s.pos[1] = y;
    s.pos[2] = z;
    s.yaw_deg = yaw;
    return s;
}

const EntityState* find_by_id(const std::vector<EntityState>& v, u32 id) {
    for (const EntityState& s : v) {
        if (s.id == id) return &s;
    }
    return nullptr;
}

}  // namespace

TEST_CASE("net-interp: midpoint sample lerps position and yaw of a shared entity",
          "[net]") {
    SnapshotInterpBuffer buf;
    // id 1 moves from (0,0,0)@yaw 0 to (10,-4,2)@yaw 90 across one second.
    buf.push(0.0, std::vector<EntityState>{make_state(1, 0.f, 0.f, 0.f, 0.f)});
    buf.push(1.0, std::vector<EntityState>{make_state(1, 10.f, -4.f, 2.f, 90.f)});

    std::vector<EntityState> out;
    REQUIRE(buf.sample(0.5, out));
    REQUIRE(out.size() == 1);
    const EntityState& s = out[0];
    CHECK(s.id == 1u);
    CHECK(s.pos[0] == Catch::Approx(5.f));
    CHECK(s.pos[1] == Catch::Approx(-2.f));
    CHECK(s.pos[2] == Catch::Approx(1.f));
    CHECK(s.yaw_deg == Catch::Approx(45.f));
}

TEST_CASE("net-interp: sampling exactly at a snapshot time returns that snapshot",
          "[net]") {
    SnapshotInterpBuffer buf;
    buf.push(0.0, std::vector<EntityState>{make_state(1, 0.f, 0.f, 0.f, 0.f)});
    buf.push(1.0, std::vector<EntityState>{make_state(1, 10.f, -4.f, 2.f, 90.f)});

    std::vector<EntityState> out;

    // Exactly at t=0 -> the first snapshot.
    REQUIRE(buf.sample(0.0, out));
    REQUIRE(out.size() == 1);
    CHECK(out[0].pos[0] == Catch::Approx(0.f));
    CHECK(out[0].yaw_deg == Catch::Approx(0.f));

    // Exactly at t=1 -> the second snapshot.
    REQUIRE(buf.sample(1.0, out));
    REQUIRE(out.size() == 1);
    CHECK(out[0].pos[0] == Catch::Approx(10.f));
    CHECK(out[0].yaw_deg == Catch::Approx(90.f));
}

TEST_CASE("net-interp: render time outside the span clamps to the endpoints (no extrapolation)",
          "[net]") {
    SnapshotInterpBuffer buf;
    buf.push(1.0, std::vector<EntityState>{make_state(1, 2.f, 0.f, 0.f, 10.f)});
    buf.push(2.0, std::vector<EntityState>{make_state(1, 8.f, 0.f, 0.f, 50.f)});

    std::vector<EntityState> out;

    // Before the oldest -> the oldest snapshot verbatim (NOT extrapolated back).
    REQUIRE(buf.sample(0.0, out));
    REQUIRE(out.size() == 1);
    CHECK(out[0].pos[0] == Catch::Approx(2.f));
    CHECK(out[0].yaw_deg == Catch::Approx(10.f));

    // After the newest -> the newest snapshot verbatim (NOT extrapolated forward).
    REQUIRE(buf.sample(99.0, out));
    REQUIRE(out.size() == 1);
    CHECK(out[0].pos[0] == Catch::Approx(8.f));
    CHECK(out[0].yaw_deg == Catch::Approx(50.f));
}

TEST_CASE("net-interp: lerp_yaw_deg takes the short way across the +/-180 wrap",
          "[net]") {
    // 170 and -170 are 20 degrees apart the SHORT way (through +/-180), 340 the
    // long way (through 0). The midpoint must be +/-180, never 0.
    const f32 mid = lerp_yaw_deg(170.f, -170.f, 0.5f);
    CHECK((mid == Catch::Approx(180.f) || mid == Catch::Approx(-180.f)));
    CHECK(std::fabs(mid) == Catch::Approx(180.f));

    // Endpoints are exact and the symmetric case wraps the other direction too.
    CHECK(lerp_yaw_deg(170.f, -170.f, 0.f) == Catch::Approx(170.f));
    CHECK(lerp_yaw_deg(170.f, -170.f, 1.f) == Catch::Approx(-170.f));
    const f32 mid2 = lerp_yaw_deg(-170.f, 170.f, 0.5f);
    CHECK(std::fabs(mid2) == Catch::Approx(180.f));

    // A plain in-range interpolation with no wrap behaves like a normal lerp.
    CHECK(lerp_yaw_deg(-45.f, 45.f, 0.5f) == Catch::Approx(0.f));
}

TEST_CASE("net-interp: an entity present in only one bracketing snapshot is held at its known value",
          "[net]") {
    SnapshotInterpBuffer buf;
    // A has ids {1,2}; B has ids {2,3}. id 1 is A-only, id 3 is B-only.
    buf.push(0.0, std::vector<EntityState>{
                      make_state(1, 1.f, 0.f, 0.f, 0.f),
                      make_state(2, 0.f, 0.f, 0.f, 0.f)});
    buf.push(1.0, std::vector<EntityState>{
                      make_state(2, 4.f, 0.f, 0.f, 20.f),
                      make_state(3, 9.f, 0.f, 0.f, 0.f)});

    std::vector<EntityState> out;
    REQUIRE(buf.sample(0.5, out));
    REQUIRE(out.size() == 3);

    // id 1 (A-only) at its known value, no extrapolation.
    const EntityState* e1 = find_by_id(out, 1);
    REQUIRE(e1 != nullptr);
    CHECK(e1->pos[0] == Catch::Approx(1.f));

    // id 2 (shared) interpolated to the midpoint.
    const EntityState* e2 = find_by_id(out, 2);
    REQUIRE(e2 != nullptr);
    CHECK(e2->pos[0] == Catch::Approx(2.f));
    CHECK(e2->yaw_deg == Catch::Approx(10.f));

    // id 3 (B-only) at its known value, no extrapolation.
    const EntityState* e3 = find_by_id(out, 3);
    REQUIRE(e3 != nullptr);
    CHECK(e3->pos[0] == Catch::Approx(9.f));
}

TEST_CASE("net-interp: capacity eviction keeps only the most recent N snapshots",
          "[net]") {
    SnapshotInterpBuffer buf(3);  // small ring
    CHECK(buf.capacity() == 3u);

    // Push 5 snapshots; only the last 3 (times 2,3,4) should survive.
    for (u32 k = 0; k < 5; ++k) {
        buf.push(static_cast<f64>(k),
                 std::vector<EntityState>{make_state(1, static_cast<f32>(k), 0.f, 0.f, 0.f)});
    }
    CHECK(buf.size() == 3u);

    std::vector<EntityState> out;

    // The evicted-away early times now clamp to the OLDEST surviving snapshot
    // (time 2, pos.x 2) — proof the older ones are gone.
    REQUIRE(buf.sample(0.0, out));
    REQUIRE(out.size() == 1);
    CHECK(out[0].pos[0] == Catch::Approx(2.f));

    // The newest survivor is time 4, pos.x 4.
    REQUIRE(buf.sample(4.0, out));
    REQUIRE(out.size() == 1);
    CHECK(out[0].pos[0] == Catch::Approx(4.f));

    // And an interior interpolation between the survivors still works.
    REQUIRE(buf.sample(2.5, out));
    REQUIRE(out.size() == 1);
    CHECK(out[0].pos[0] == Catch::Approx(2.5f));
}

TEST_CASE("net-interp: an empty buffer sample returns false and clears out",
          "[net]") {
    SnapshotInterpBuffer buf;
    CHECK(buf.empty());

    std::vector<EntityState> out{make_state(7, 1.f, 2.f, 3.f, 4.f)};  // pre-filled
    CHECK_FALSE(buf.sample(0.5, out));
    CHECK(out.empty());  // sample must have cleared it
}

TEST_CASE("net-interp: sample output is in ascending id order",
          "[net]") {
    SnapshotInterpBuffer buf;
    // Push ids deliberately out of order; the buffer must still emit ascending.
    buf.push(0.0, std::vector<EntityState>{
                      make_state(30, 0.f, 0.f, 0.f, 0.f),
                      make_state(5, 0.f, 0.f, 0.f, 0.f),
                      make_state(17, 0.f, 0.f, 0.f, 0.f)});
    buf.push(1.0, std::vector<EntityState>{
                      make_state(17, 2.f, 0.f, 0.f, 0.f),
                      make_state(5, 2.f, 0.f, 0.f, 0.f),
                      make_state(30, 2.f, 0.f, 0.f, 0.f)});

    std::vector<EntityState> out;
    REQUIRE(buf.sample(0.5, out));
    REQUIRE(out.size() == 3);
    CHECK(out[0].id == 5u);
    CHECK(out[1].id == 17u);
    CHECK(out[2].id == 30u);
}

TEST_CASE("net-interp: stale out-of-order push is ignored; duplicate timestamp replaces",
          "[net]") {
    SnapshotInterpBuffer buf;
    buf.push(0.0, std::vector<EntityState>{make_state(1, 0.f, 0.f, 0.f, 0.f)});
    buf.push(2.0, std::vector<EntityState>{make_state(1, 20.f, 0.f, 0.f, 0.f)});

    // A strictly-older push is ignored (would corrupt the time-sorted ring).
    buf.push(1.0, std::vector<EntityState>{make_state(1, 999.f, 0.f, 0.f, 0.f)});
    CHECK(buf.size() == 2u);

    // A duplicate-timestamp push replaces the newest entry in place.
    buf.push(2.0, std::vector<EntityState>{make_state(1, 30.f, 0.f, 0.f, 0.f)});
    CHECK(buf.size() == 2u);

    std::vector<EntityState> out;
    REQUIRE(buf.sample(2.0, out));
    REQUIRE(out.size() == 1);
    CHECK(out[0].pos[0] == Catch::Approx(30.f));  // the replacement value
}

TEST_CASE("net-interp: identical push/sample sequences yield bit-identical output",
          "[net][determinism]") {
    auto run = []() {
        SnapshotInterpBuffer buf(8);
        buf.push(0.0, std::vector<EntityState>{
                          make_state(1, 0.f, 1.f, 2.f, 170.f),
                          make_state(2, -3.f, 4.f, 5.f, -45.f)});
        buf.push(0.5, std::vector<EntityState>{
                          make_state(1, 1.f, 1.5f, 2.5f, -170.f),
                          make_state(2, -1.f, 4.f, 6.f, 45.f),
                          make_state(9, 7.f, 8.f, 9.f, 0.f)});
        buf.push(1.0, std::vector<EntityState>{
                          make_state(2, 0.f, 4.f, 7.f, 90.f),
                          make_state(9, 7.5f, 8.f, 9.f, 10.f)});
        std::vector<EntityState> out;
        // A render time inside both intervals exercises interp + single-side.
        buf.sample(0.37, out);
        return out;
    };

    const std::vector<EntityState> a = run();
    const std::vector<EntityState> b = run();
    REQUIRE(a.size() == b.size());
    // Byte-for-byte identical (EntityState is trivially copyable / POD).
    CHECK(std::memcmp(a.data(), b.data(), a.size() * sizeof(EntityState)) == 0);
}
