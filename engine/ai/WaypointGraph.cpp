// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/ai/WaypointGraph.cpp — see WaypointGraph.h.

#include "ai/WaypointGraph.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace psynder::ai {

namespace {
constexpr f32 kInf = std::numeric_limits<f32>::infinity();

// Straight-line distance between two world-space points: pure algebra + one
// sqrt (deterministic under strict-FP).
f32 distance(math::Vec3 a, math::Vec3 b) noexcept {
    const f32 dx = a.x - b.x;
    const f32 dy = a.y - b.y;
    const f32 dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}
}  // namespace

u32 WaypointGraph::add_node(math::Vec3 pos) {
    const u32 index = static_cast<u32>(nodes_.size());
    nodes_.push_back(pos);
    adj_.emplace_back();
    return index;
}

void WaypointGraph::add_edge(u32 a, u32 b, bool bidirectional) {
    const usize n = nodes_.size();
    if (a >= n || b >= n) return;  // ignore out-of-range
    const f32 cost = distance(nodes_[a], nodes_[b]);
    adj_[a].push_back(Edge{b, cost});
    if (bidirectional) adj_[b].push_back(Edge{a, cost});
}

math::Vec3 WaypointGraph::node_pos(u32 i) const noexcept {
    if (i >= nodes_.size()) return math::Vec3{0.0f, 0.0f, 0.0f};
    return nodes_[i];
}

bool WaypointGraph::nearest_node(math::Vec3 pos, u32& out_node) const noexcept {
    const usize n = nodes_.size();
    if (n == 0) return false;

    u32 best = 0;
    // Compare squared distance to avoid a sqrt; ties keep the lowest index
    // because we only replace on a strictly smaller distance.
    f32 best_d2 = kInf;
    for (usize i = 0; i < n; ++i) {
        const f32 dx = pos.x - nodes_[i].x;
        const f32 dy = pos.y - nodes_[i].y;
        const f32 dz = pos.z - nodes_[i].z;
        const f32 d2 = dx * dx + dy * dy + dz * dz;
        if (d2 < best_d2) {
            best_d2 = d2;
            best = static_cast<u32>(i);
        }
    }
    out_node = best;
    return true;
}

bool WaypointGraph::find_path(u32 start, u32 goal,
                              std::vector<u32>& out_nodes) const {
    out_nodes.clear();
    const usize n = nodes_.size();
    if (start >= n || goal >= n) return false;

    if (start == goal) {
        out_nodes.push_back(start);
        return true;
    }

    // Size + clear the reused scratch to the current node count.
    g_.assign(n, kInf);
    f_.assign(n, kInf);
    parent_.assign(n, std::numeric_limits<u32>::max());
    open_.assign(n, 0u);
    closed_.assign(n, 0u);
    heap_.clear();

    const math::Vec3 goal_pos = nodes_[goal];

    // (f_cost, node_index) ordering: a < b iff f[a] < f[b], or equal f and a
    // smaller index. Ties break by lowest node index -> deterministic. We never
    // rely on std::priority_queue's unspecified equal-key ordering.
    auto less = [this](u32 a, u32 b) noexcept -> bool {
        if (f_[a] != f_[b]) return f_[a] < f_[b];
        return a < b;
    };
    auto push = [&](u32 node) {
        heap_.push_back(node);
        usize i = heap_.size() - 1;
        while (i > 0) {
            const usize parent = (i - 1) / 2;
            if (less(heap_[i], heap_[parent])) {
                std::swap(heap_[i], heap_[parent]);
                i = parent;
            } else {
                break;
            }
        }
    };
    auto pop = [&]() -> u32 {
        const u32 top = heap_[0];
        heap_[0] = heap_.back();
        heap_.pop_back();
        const usize sz = heap_.size();
        usize i = 0;
        for (;;) {
            const usize l = 2 * i + 1;
            const usize r = 2 * i + 2;
            usize smallest = i;
            if (l < sz && less(heap_[l], heap_[smallest])) smallest = l;
            if (r < sz && less(heap_[r], heap_[smallest])) smallest = r;
            if (smallest == i) break;
            std::swap(heap_[i], heap_[smallest]);
            i = smallest;
        }
        return top;
    };

    g_[start] = 0.0f;
    f_[start] = distance(nodes_[start], goal_pos);
    open_[start] = 1u;
    push(start);

    while (!heap_.empty()) {
        const u32 cur = pop();
        if (!open_[cur]) continue;  // stale heap entry for an already-popped node
        open_[cur] = 0u;

        if (cur == goal) {
            // Reconstruct start..goal (inclusive) by walking parents, then flip.
            u32 c = goal;
            while (c != std::numeric_limits<u32>::max()) {
                out_nodes.push_back(c);
                if (c == start) break;
                c = parent_[c];
            }
            std::reverse(out_nodes.begin(), out_nodes.end());
            return true;
        }

        closed_[cur] = 1u;
        const f32 gc = g_[cur];

        // Fixed adjacency scan order (insertion order) keeps ties deterministic.
        for (const Edge& e : adj_[cur]) {
            if (closed_[e.to]) continue;
            const f32 ng = gc + e.cost;
            if (ng < g_[e.to]) {
                g_[e.to] = ng;
                parent_[e.to] = cur;
                f_[e.to] = ng + distance(nodes_[e.to], goal_pos);
                open_[e.to] = 1u;
                push(e.to);  // lazy-deletion: an older worse entry stays but is
                             // skipped via the open_/stale check on pop.
            }
        }
    }

    out_nodes.clear();
    return false;
}

}  // namespace psynder::ai
