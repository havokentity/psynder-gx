// SPDX-License-Identifier: MIT
// Psynder — RoomGraph: convex-room arena + portal-connectivity PVS. Lane 10.
// See RoomGraph.h for the PVS model and the clipped-frustum simplification.

#include "world/bsp/RoomGraph.h"

#include <algorithm>
#include <vector>

namespace psynder::world::bsp {

namespace {

// Shared empty result for out-of-range visible_set() queries — keeps the API
// allocation-free and reference-returning without dangling.
const std::vector<u32>& empty_set() {
    static const std::vector<u32> kEmpty;
    return kEmpty;
}

// Inclusive AABB containment (closed box). Authoring assumes min <= max.
bool contains(const Room& r, math::Vec3 p) noexcept {
    return p.x >= r.min.x && p.x <= r.max.x &&
           p.y >= r.min.y && p.y <= r.max.y &&
           p.z >= r.min.z && p.z <= r.max.z;
}

}  // namespace

u32 RoomGraph::add_room(const Room& room) {
    const u32 index = static_cast<u32>(rooms_.size());
    rooms_.push_back(room);
    adjacency_.emplace_back();
    visible_.emplace_back();  // empty until build_pvs()
    return index;
}

void RoomGraph::add_portal(u32 room_a, u32 room_b) {
    const u32 count = static_cast<u32>(rooms_.size());
    if (room_a >= count || room_b >= count) return;  // out of range
    if (room_a == room_b) return;                     // a portal needs two rooms

    // Undirected: record both directions. Skip duplicates so adjacency stays a
    // clean set (flood-fill is correct either way, but this keeps it tidy).
    auto& na = adjacency_[room_a];
    if (std::find(na.begin(), na.end(), room_b) == na.end()) na.push_back(room_b);
    auto& nb = adjacency_[room_b];
    if (std::find(nb.begin(), nb.end(), room_a) == nb.end()) nb.push_back(room_a);
}

void RoomGraph::build_pvs() {
    const usize n = rooms_.size();
    visible_.assign(n, {});

    // Iterative BFS/DFS flood per room over the portal adjacency. Each room's
    // PVS is its connected component; we sort ascending for determinism.
    std::vector<u8> seen(n, 0u);     // reused scratch, cleared per source room
    std::vector<usize> stack;        // explicit stack (no recursion depth limit)

    for (usize src = 0; src < n; ++src) {
        std::fill(seen.begin(), seen.end(), u8{0});
        stack.clear();

        seen[src] = 1u;
        stack.push_back(src);
        while (!stack.empty()) {
            const usize cur = stack.back();
            stack.pop_back();
            for (const u32 nbr : adjacency_[cur]) {
                if (!seen[nbr]) {
                    seen[nbr] = 1u;
                    stack.push_back(nbr);
                }
            }
        }

        // Collect the component in ascending index order (deterministic).
        auto& out = visible_[src];
        for (usize i = 0; i < n; ++i) {
            if (seen[i]) out.push_back(static_cast<u32>(i));
        }
    }
}

i32 RoomGraph::room_at(math::Vec3 point) const {
    const usize n = rooms_.size();
    for (usize i = 0; i < n; ++i) {
        if (contains(rooms_[i], point)) return static_cast<i32>(i);  // lowest wins
    }
    return -1;
}

bool RoomGraph::potentially_visible(u32 from_room, u32 to_room) const {
    if (from_room >= visible_.size()) return false;
    const auto& set = visible_[from_room];
    // visible_[from_room] is sorted ascending → binary search.
    return std::binary_search(set.begin(), set.end(), to_room);
}

const std::vector<u32>& RoomGraph::visible_set(u32 room) const {
    if (room >= visible_.size()) return empty_set();
    return visible_[room];
}

}  // namespace psynder::world::bsp
