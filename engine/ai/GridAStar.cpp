// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/ai/GridAStar.cpp — see GridAStar.h.

#include "ai/GridAStar.h"

#include <algorithm>

namespace psynder::ai {

namespace {
// 8-neighbour offsets + integer edge costs (cardinal 10, diagonal 14 ~= 10*sqrt2).
// Same fixed scan order as FlowField for matching determinism.
constexpr int kDX[8] = {1, -1, 0, 0, 1, 1, -1, -1};
constexpr int kDZ[8] = {0, 0, 1, -1, 1, -1, 1, -1};
constexpr u32 kCost[8] = {10, 10, 10, 10, 14, 14, 14, 14};

constexpr u32 kInf = 0xFFFFFFFFu;

// Admissible integer octile heuristic between two cells.
u32 octile(u32 ax, u32 az, u32 bx, u32 bz) noexcept {
    const u32 dx = (ax > bx) ? (ax - bx) : (bx - ax);
    const u32 dz = (az > bz) ? (az - bz) : (bz - az);
    const u32 lo = (dx < dz) ? dx : dz;
    const u32 hi = (dx < dz) ? dz : dx;
    return 14u * lo + 10u * (hi - lo);
}
}  // namespace

void GridAStar::resize(u32 width, u32 height) {
    w_ = width;
    h_ = height;
    const usize n = static_cast<usize>(w_) * h_;
    blocked_.assign(n, 0u);
    g_.assign(n, kInf);
    parent_.assign(n, kInf);
    closed_.assign(n, 0u);
    open_.assign(n, 0u);
    f_.assign(n, kInf);
    heap_.clear();
}

void GridAStar::block_all_clear() {
    std::fill(blocked_.begin(), blocked_.end(), u8{0});
}

void GridAStar::set_blocked(u32 x, u32 z, bool blocked) {
    if (x >= w_ || z >= h_) return;
    blocked_[static_cast<usize>(z) * w_ + x] = blocked ? 1u : 0u;
}

bool GridAStar::blocked(u32 x, u32 z) const noexcept {
    if (x >= w_ || z >= h_) return true;
    return blocked_[static_cast<usize>(z) * w_ + x] != 0u;
}

bool GridAStar::find_path(u32 start_x, u32 start_z, u32 goal_x, u32 goal_z,
                          std::vector<u32>& out_cells) {
    out_cells.clear();
    if (w_ == 0 || h_ == 0) return false;
    if (start_x >= w_ || start_z >= h_ || goal_x >= w_ || goal_z >= h_)
        return false;

    const u32 start = start_z * w_ + start_x;
    const u32 goal = goal_z * w_ + goal_x;
    if (blocked_[start] || blocked_[goal]) return false;

    // Reset only the cells we touched last time would be ideal, but a full clear
    // keeps the search deterministic and is O(w*h) which is the search's bound
    // anyway. std::fill is branch-free and cache-friendly here.
    std::fill(g_.begin(), g_.end(), kInf);
    std::fill(parent_.begin(), parent_.end(), kInf);
    std::fill(closed_.begin(), closed_.end(), u8{0});
    std::fill(open_.begin(), open_.end(), u8{0});
    std::fill(f_.begin(), f_.end(), kInf);
    heap_.clear();

    // (f_cost, cell_index) ordering: a < b iff f[a] < f[b], or equal f and
    // smaller index. Ties break by lowest cell index -> deterministic.
    auto less = [this](u32 a, u32 b) noexcept -> bool {
        if (f_[a] != f_[b]) return f_[a] < f_[b];
        return a < b;
    };
    auto push = [&](u32 cell) {
        heap_.push_back(cell);
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

    g_[start] = 0u;
    f_[start] = octile(start_x, start_z, goal_x, goal_z);
    open_[start] = 1u;
    push(start);

    while (!heap_.empty()) {
        const u32 cur = pop();
        if (!open_[cur]) continue;  // stale heap entry for an already-popped cell
        open_[cur] = 0u;

        if (cur == goal) {
            // Reconstruct start..goal (inclusive) by walking parents, then flip.
            u32 c = goal;
            while (c != kInf) {
                out_cells.push_back(c);
                if (c == start) break;
                c = parent_[c];
            }
            std::reverse(out_cells.begin(), out_cells.end());
            return true;
        }

        closed_[cur] = 1u;
        const u32 cx = cur % w_;
        const u32 cz = cur / w_;
        const u32 gc = g_[cur];

        for (int k = 0; k < 8; ++k) {
            const int nx = static_cast<int>(cx) + kDX[k];
            const int nz = static_cast<int>(cz) + kDZ[k];
            if (nx < 0 || nz < 0 || nx >= static_cast<int>(w_) ||
                nz >= static_cast<int>(h_))
                continue;
            const u32 ni = static_cast<u32>(nz) * w_ + static_cast<u32>(nx);
            if (blocked_[ni] || closed_[ni]) continue;
            if (k >= 4) {  // diagonal: don't cut a blocked corner
                const u32 oa = cz * w_ + static_cast<u32>(nx);
                const u32 ob = static_cast<u32>(nz) * w_ + cx;
                if (blocked_[oa] || blocked_[ob]) continue;
            }
            const u32 ng = gc + kCost[k];
            if (ng < g_[ni]) {
                g_[ni] = ng;
                parent_[ni] = cur;
                f_[ni] = ng + octile(static_cast<u32>(nx), static_cast<u32>(nz),
                                     goal_x, goal_z);
                open_[ni] = 1u;
                push(ni);  // lazy-deletion: an old worse entry stays but is
                           // skipped via the open_/stale check on pop.
            }
        }
    }

    out_cells.clear();
    return false;
}

}  // namespace psynder::ai
