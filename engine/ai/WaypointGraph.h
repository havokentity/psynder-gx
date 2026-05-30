// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/ai/WaypointGraph.h
//
// A sparse navigation waypoint graph — world-space nodes connected by edges,
// for outdoor / objective-style navigation where a full GridAStar grid would be
// wasteful (long open stretches, a handful of corridors and objective markers).
// Where GridAStar bakes a dense regular grid, WaypointGraph stores only the
// nodes that matter and the explicit links between them, then A*'s a node path
// between any two of them.
//
// Determinism (lockstep pillar): edge costs are the Euclidean distance between
// node positions (pure +,-,*,/ plus one sqrt); the A* search runs over a binary
// min-heap keyed by (f_cost, node_index) so equal-f ties break by lowest node
// index — no reliance on std::priority_queue's unspecified equal-key ordering
// and no RNG / trig / platform branches. Same graph + endpoints => a
// bit-identical out_nodes on every run/platform. Internal A* scratch is sized to
// node_count() and reused across calls. Built -fno-fast-math -ffp-contract=off.

#pragma once

#include "math/Math.h"

#include "core/Types.h"

#include <vector>

namespace psynder::ai {

class WaypointGraph {
public:
    // Add a world-space node at `pos`; returns its node index (0-based, stable).
    u32 add_node(math::Vec3 pos);

    // Connect nodes `a` and `b` with an edge whose cost is the Euclidean
    // distance between their positions. When `bidirectional` is true (the
    // default) the reverse edge is added too. Out-of-range indices are ignored.
    void add_edge(u32 a, u32 b, bool bidirectional = true);

    usize node_count() const noexcept { return nodes_.size(); }

    // World-space position of node `i`, or {0,0,0} when `i` is out of range.
    math::Vec3 node_pos(u32 i) const noexcept;

    // The node with the smallest straight-line distance to `pos`; ties break by
    // lowest node index. Writes the index to `out_node` and returns true; returns
    // false (and leaves `out_node` untouched) when the graph is empty.
    bool nearest_node(math::Vec3 pos, u32& out_node) const noexcept;

    // A* a node path from `start` to `goal` (g = sum of edge distances,
    // h = straight-line distance to the goal node). Fills `out_nodes` with the
    // node-index path start..goal inclusive and returns true when a route exists;
    // returns false and leaves `out_nodes` empty when the goal is unreachable or
    // an endpoint is out of range. start == goal yields a single-node path.
    // Deterministic: identical graph + endpoints => identical out_nodes.
    bool find_path(u32 start, u32 goal, std::vector<u32>& out_nodes) const;

private:
    struct Edge {
        u32 to;
        f32 cost;
    };

    std::vector<math::Vec3>         nodes_;  // node positions
    std::vector<std::vector<Edge>>  adj_;    // adjacency, per node

    // Reused A* scratch (resized to node_count() on each find_path call).
    mutable std::vector<f32>  g_;       // best known g-cost per node (kInf unseen)
    mutable std::vector<f32>  f_;       // f-cost per node (the heap order key)
    mutable std::vector<u32>  parent_;  // predecessor node on the best path
    mutable std::vector<u8>   open_;    // 1 while a node sits in the open heap
    mutable std::vector<u8>   closed_;  // 1 once a node is finalised
    mutable std::vector<u32>  heap_;    // binary min-heap of node indices
};

}  // namespace psynder::ai
