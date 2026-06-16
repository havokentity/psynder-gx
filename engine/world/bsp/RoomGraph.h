// SPDX-License-Identifier: MIT
// Psynder — convex-room arena graph with portal-connectivity PVS. Lane 10
// (world-bsp) owns. Headless authoring/geometry complement to the BSP tree.
//
// RoomGraph is the simple, fully-unit-testable convex-room sibling of the
// general BSP/portal/PVS pipeline (Bsp.h / Portal.h / Pvs.h). Where the BSP
// path derives leaves + portals from arbitrary brush geometry and clips view
// frusta through portal windings, RoomGraph models a Quake3-style indoor map
// as a hand-authored set of *axis-aligned convex rooms* connected by *portals*
// (shared openings). It answers three questions a level needs before any
// rendering or AI:
//
//   1. point-location  — which room contains a world point? (room_at)
//   2. room PVS         — which rooms are potentially visible from a room?
//   3. visibility query — is room B potentially visible from room A?
//
// PVS MODEL (and its simplification)
// ----------------------------------
// The portals define an undirected adjacency graph over rooms. build_pvs()
// floods that graph: a room's potentially-visible set is its entire connected
// component (itself + every room reachable through any chain of portals). A
// sealed room (no portals) sees only itself.
//
// This is the *connectivity upper bound* on visibility — it is exactly what a
// real PVS can never exceed, but it is looser than a real PVS. A production
// portal-PVS clips the view frustum at each portal opening: a room reachable
// only through a portal that falls entirely outside the running clipped frustum
// is NOT actually visible and would be culled. RoomGraph deliberately omits
// that geometric clipping — it returns the topological reachability set, which
// is correct (never hides a truly-visible room) and conservative (may include
// rooms a frustum-clipping PVS would reject). Bsp.h's portal path is where the
// clipped-frustum tightening lives; this is the cheap, deterministic skeleton.
//
// Deterministic: outputs (visible sets) are sorted ascending; rebuilding from
// the same topology yields bit-identical results. Allocation-conscious: this is
// offline/authoring map data, so build_pvs() may allocate, but the query path
// (room_at / potentially_visible / visible_set) does not.

#pragma once

#include "core/Types.h"
#include "math/Math.h"

#include <vector>

namespace psynder::world::bsp {

// An axis-aligned convex room volume. `min`/`max` are the world-space corners
// (1 unit = 1 metre); min <= max component-wise is assumed (the authoring tool
// is the only writer). A point p is inside iff
//   min.x <= p.x <= max.x  &&  min.y <= p.y <= max.y  &&  min.z <= p.z <= max.z.
struct Room {
    math::Vec3 min;
    math::Vec3 max;
};

// Convex-room arena graph. Build topology with add_room/add_portal, then call
// build_pvs() once; thereafter the queries are read-only and allocation-free.
class RoomGraph {
public:
    RoomGraph() = default;

    // Append a room; returns its stable index (== prior room_count()). Adding a
    // room after build_pvs() invalidates the PVS until build_pvs() is re-run.
    u32 add_room(const Room& room);

    // Connect two rooms with a portal (a shared opening). Undirected: the
    // adjacency is symmetric. Out-of-range or self (a == b) portals are ignored
    // defensively. Duplicate portals are harmless (flood-fill is idempotent).
    void add_portal(u32 room_a, u32 room_b);

    // Flood the portal graph: each room's potentially-visible set becomes its
    // whole connected component (itself + everything reachable through portals).
    // Sets are stored ascending. This is the only call that allocates beyond
    // add_room/add_portal. Idempotent: re-running yields identical results.
    void build_pvs();

    // Index of the room whose AABB contains `point`, or -1 if no room does. On
    // overlap (a point inside several rooms) the lowest room index wins, keeping
    // the result deterministic. O(room_count); no allocation.
    i32 room_at(math::Vec3 point) const;

    // True iff `to_room` is in `from_room`'s built PVS. Reads the table computed
    // by build_pvs(); out-of-range indices return false. A room is always
    // potentially visible from itself once built. No allocation.
    bool potentially_visible(u32 from_room, u32 to_room) const;

    // The ascending list of rooms potentially visible from `room` (its PVS). For
    // an out-of-range room, returns a shared empty vector. No allocation.
    const std::vector<u32>& visible_set(u32 room) const;

    // Number of rooms added so far.
    usize room_count() const noexcept { return rooms_.size(); }

private:
    std::vector<Room>             rooms_;
    std::vector<std::vector<u32>> adjacency_;    // per-room portal neighbours
    std::vector<std::vector<u32>> visible_;       // per-room PVS (ascending)
};

}  // namespace psynder::world::bsp
