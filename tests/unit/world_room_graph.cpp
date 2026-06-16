// SPDX-License-Identifier: MIT
// Psynder — Lane 10 (world-bsp) RoomGraph unit test.
//
// Convex-room arena with portal-connectivity PVS. The canonical fixture is a
// row of three rooms A-B-C plus a sealed room D:
//
//   A [0..10]  --portal--  B [10..20]  --portal--  C [20..30]      D [100..110]
//
// Portals connect A<->B and B<->C but NOT A<->C. After build_pvs():
//   - A's PVS = {A, B, C}   (C reachable through B; this is the connectivity
//                            upper bound — a real frustum-clipping PVS could be
//                            tighter, see RoomGraph.h)
//   - D's PVS = {D}         (sealed room sees only itself)
// Point-location maps interior points to their room and outside points to -1.

#include <catch2/catch_test_macros.hpp>

#include <vector>

#include "world/bsp/RoomGraph.h"

using namespace psynder;
using namespace psynder::world::bsp;

namespace {

constexpr u32 kRoomA = 0u;
constexpr u32 kRoomB = 1u;
constexpr u32 kRoomC = 2u;
constexpr u32 kRoomD = 3u;

// Build the A-B-C row (+ optionally the B<->C portal) plus a sealed room D.
// When `link_bc` is false, only A<->B is wired so C is its own island.
RoomGraph make_arena(bool link_bc) {
    RoomGraph g;
    // A: x in [0,10], B: [10,20], C: [20,30] — adjacent boxes sharing a face,
    // unit-height/depth so interior test points are unambiguous.
    const u32 a = g.add_room(Room{math::Vec3{0.0f, 0.0f, 0.0f},   math::Vec3{10.0f, 4.0f, 4.0f}});
    const u32 b = g.add_room(Room{math::Vec3{10.0f, 0.0f, 0.0f},  math::Vec3{20.0f, 4.0f, 4.0f}});
    const u32 c = g.add_room(Room{math::Vec3{20.0f, 0.0f, 0.0f},  math::Vec3{30.0f, 4.0f, 4.0f}});
    // D: far away and sealed (no portals).
    const u32 d = g.add_room(Room{math::Vec3{100.0f, 0.0f, 0.0f}, math::Vec3{110.0f, 4.0f, 4.0f}});

    REQUIRE(a == kRoomA);
    REQUIRE(b == kRoomB);
    REQUIRE(c == kRoomC);
    REQUIRE(d == kRoomD);

    g.add_portal(kRoomA, kRoomB);
    if (link_bc) g.add_portal(kRoomB, kRoomC);
    return g;
}

}  // namespace

TEST_CASE("world_room_graph flood reaches through chained portals", "[world_bsp]") {
    RoomGraph g = make_arena(/*link_bc=*/true);
    REQUIRE(g.room_count() == 4u);

    g.build_pvs();

    // A reaches C through B even with no direct A<->C portal.
    REQUIRE(g.visible_set(kRoomA) == std::vector<u32>{kRoomA, kRoomB, kRoomC});
    REQUIRE(g.visible_set(kRoomB) == std::vector<u32>{kRoomA, kRoomB, kRoomC});
    REQUIRE(g.visible_set(kRoomC) == std::vector<u32>{kRoomA, kRoomB, kRoomC});

    // Connectivity is symmetric for connected rooms.
    REQUIRE(g.potentially_visible(kRoomA, kRoomC));
    REQUIRE(g.potentially_visible(kRoomC, kRoomA));
    REQUIRE(g.potentially_visible(kRoomA, kRoomB));
    REQUIRE(g.potentially_visible(kRoomB, kRoomA));

    // A room always sees itself once built.
    REQUIRE(g.potentially_visible(kRoomA, kRoomA));
}

TEST_CASE("world_room_graph sealed room sees only itself", "[world_bsp]") {
    RoomGraph g = make_arena(/*link_bc=*/true);
    g.build_pvs();

    REQUIRE(g.visible_set(kRoomD) == std::vector<u32>{kRoomD});
    REQUIRE(g.potentially_visible(kRoomD, kRoomD));

    // D is not reachable from the A-B-C component, and vice versa (symmetric).
    REQUIRE_FALSE(g.potentially_visible(kRoomD, kRoomA));
    REQUIRE_FALSE(g.potentially_visible(kRoomA, kRoomD));
    REQUIRE_FALSE(g.potentially_visible(kRoomD, kRoomB));
    REQUIRE_FALSE(g.potentially_visible(kRoomC, kRoomD));
}

TEST_CASE("world_room_graph without the B-C portal C is its own island",
          "[world_bsp]") {
    RoomGraph g = make_arena(/*link_bc=*/false);  // only A<->B
    g.build_pvs();

    // A and B form one component; C is isolated (and D too).
    REQUIRE(g.visible_set(kRoomA) == std::vector<u32>{kRoomA, kRoomB});
    REQUIRE(g.visible_set(kRoomB) == std::vector<u32>{kRoomA, kRoomB});
    REQUIRE(g.visible_set(kRoomC) == std::vector<u32>{kRoomC});
    REQUIRE(g.visible_set(kRoomD) == std::vector<u32>{kRoomD});

    REQUIRE(g.potentially_visible(kRoomA, kRoomB));
    REQUIRE_FALSE(g.potentially_visible(kRoomA, kRoomC));
    REQUIRE_FALSE(g.potentially_visible(kRoomC, kRoomA));
    REQUIRE_FALSE(g.potentially_visible(kRoomB, kRoomC));
}

TEST_CASE("world_room_graph point location maps points to rooms", "[world_bsp]") {
    RoomGraph g = make_arena(/*link_bc=*/true);
    g.build_pvs();  // point-location is independent of PVS but harmless here

    // Interior points (well inside each box, away from shared faces).
    REQUIRE(g.room_at(math::Vec3{5.0f,  2.0f, 2.0f})   == static_cast<i32>(kRoomA));
    REQUIRE(g.room_at(math::Vec3{15.0f, 2.0f, 2.0f})   == static_cast<i32>(kRoomB));
    REQUIRE(g.room_at(math::Vec3{25.0f, 2.0f, 2.0f})   == static_cast<i32>(kRoomC));
    REQUIRE(g.room_at(math::Vec3{105.0f, 2.0f, 2.0f})  == static_cast<i32>(kRoomD));

    // Outside every room → -1.
    REQUIRE(g.room_at(math::Vec3{-5.0f, 2.0f, 2.0f})   == -1);
    REQUIRE(g.room_at(math::Vec3{50.0f, 2.0f, 2.0f})   == -1);   // gap between C and D
    REQUIRE(g.room_at(math::Vec3{5.0f, 100.0f, 2.0f})  == -1);   // above the arena
}

TEST_CASE("world_room_graph overlap resolves to the lowest room index",
          "[world_bsp]") {
    RoomGraph g;
    // Two rooms sharing the volume around the origin; lowest index must win.
    const u32 lo = g.add_room(Room{math::Vec3{0.0f, 0.0f, 0.0f}, math::Vec3{10.0f, 10.0f, 10.0f}});
    const u32 hi = g.add_room(Room{math::Vec3{5.0f, 5.0f, 5.0f}, math::Vec3{15.0f, 15.0f, 15.0f}});
    REQUIRE(lo == 0u);
    REQUIRE(hi == 1u);
    g.build_pvs();

    // Point inside both boxes → the lower index (0).
    REQUIRE(g.room_at(math::Vec3{7.0f, 7.0f, 7.0f}) == 0);
    // Point only inside the higher-index box.
    REQUIRE(g.room_at(math::Vec3{12.0f, 12.0f, 12.0f}) == 1);
}

TEST_CASE("world_room_graph visible sets are ascending", "[world_bsp]") {
    // Wire portals in reverse / scrambled order; the PVS must still come out
    // ascending. Rooms 0..3 fully connected via 3<->2, 2<->1, 1<->0.
    RoomGraph g;
    for (int i = 0; i < 4; ++i) {
        const f32 base = static_cast<f32>(i) * 10.0f;
        g.add_room(Room{math::Vec3{base, 0.0f, 0.0f}, math::Vec3{base + 10.0f, 4.0f, 4.0f}});
    }
    g.add_portal(3u, 2u);
    g.add_portal(2u, 1u);
    g.add_portal(1u, 0u);
    g.build_pvs();

    for (u32 r = 0; r < static_cast<u32>(g.room_count()); ++r) {
        const std::vector<u32>& set = g.visible_set(r);
        REQUIRE(set == std::vector<u32>{0u, 1u, 2u, 3u});
        // Explicitly assert strictly ascending.
        for (usize k = 1; k < set.size(); ++k) {
            REQUIRE(set[k - 1] < set[k]);
        }
    }
}

TEST_CASE("world_room_graph rebuild is deterministic", "[world_bsp]") {
    RoomGraph g = make_arena(/*link_bc=*/true);

    g.build_pvs();
    std::vector<std::vector<u32>> first;
    for (u32 r = 0; r < static_cast<u32>(g.room_count()); ++r) {
        first.push_back(g.visible_set(r));
    }

    // Rebuilding from identical topology yields identical PVS.
    g.build_pvs();
    for (u32 r = 0; r < static_cast<u32>(g.room_count()); ++r) {
        REQUIRE(g.visible_set(r) == first[r]);
    }

    // A freshly-constructed graph with the same topology matches too.
    RoomGraph h = make_arena(/*link_bc=*/true);
    h.build_pvs();
    REQUIRE(h.room_count() == g.room_count());
    for (u32 r = 0; r < static_cast<u32>(h.room_count()); ++r) {
        REQUIRE(h.visible_set(r) == first[r]);
    }
}

TEST_CASE("world_room_graph defensive portal and query guards", "[world_bsp]") {
    RoomGraph g = make_arena(/*link_bc=*/true);

    // Out-of-range / self portals are ignored and must not perturb the PVS.
    g.add_portal(kRoomA, 99u);   // out of range
    g.add_portal(7u, 8u);        // both out of range
    g.add_portal(kRoomD, kRoomD);// self-portal: a room cannot portal to itself
    g.build_pvs();

    // D stayed sealed despite the self-portal attempt.
    REQUIRE(g.visible_set(kRoomD) == std::vector<u32>{kRoomD});
    REQUIRE(g.visible_set(kRoomA) == std::vector<u32>{kRoomA, kRoomB, kRoomC});

    // Out-of-range queries are safe.
    REQUIRE_FALSE(g.potentially_visible(99u, kRoomA));
    REQUIRE_FALSE(g.potentially_visible(kRoomA, 99u));
    REQUIRE(g.visible_set(99u).empty());
}
