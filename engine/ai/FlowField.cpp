// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/ai/FlowField.cpp — see FlowField.h.

#include "ai/FlowField.h"

#include <algorithm>
#include <cmath>
#include <queue>
#include <utility>

namespace psynder::ai {

namespace {
// 8-neighbour offsets + integer edge costs (cardinal 10, diagonal 14 ~= 10*sqrt2).
constexpr int kDX[8] = {1, -1, 0, 0, 1, 1, -1, -1};
constexpr int kDZ[8] = {0, 0, 1, -1, 1, -1, 1, -1};
constexpr u32 kCost[8] = {10, 10, 10, 10, 14, 14, 14, 14};
}  // namespace

void FlowField::resize(math::Vec3 origin, f32 cell_size, u32 width, u32 height) {
    origin_ = origin;
    cell_ = (cell_size > 0.0f) ? cell_size : 1.0f;
    w_ = width;
    h_ = height;
    const usize n = static_cast<usize>(w_) * h_;
    blocked_.assign(n, 0u);
    cost_.assign(n, kUnreachable);
    flow_.assign(n, math::Vec3{0.0f, 0.0f, 0.0f});
}

void FlowField::clear_blocks() { std::fill(blocked_.begin(), blocked_.end(), u8{0}); }

bool FlowField::to_cell(math::Vec3 world, u32& cx, u32& cz) const noexcept {
    const f32 fx = (world.x - origin_.x) / cell_;
    const f32 fz = (world.z - origin_.z) / cell_;
    if (fx < 0.0f || fz < 0.0f) return false;
    const u32 ix = static_cast<u32>(fx);
    const u32 iz = static_cast<u32>(fz);
    if (ix >= w_ || iz >= h_) return false;
    cx = ix;
    cz = iz;
    return true;
}

void FlowField::block_aabb(const math::Aabb& box) {
    if (w_ == 0 || h_ == 0) return;
    // Cells with POSITIVE overlap with the box's XZ footprint: the min edge maps
    // to floor, the max edge to ceil-1, so a box ending exactly on a grid line
    // does not block the next cell (half-open cells [c, c+1)). Clamped to grid.
    auto lo = [&](f32 v, f32 o) { return static_cast<i64>(std::floor((v - o) / cell_)); };
    auto hi = [&](f32 v, f32 o) { return static_cast<i64>(std::ceil((v - o) / cell_)) - 1; };
    const i64 x0 = std::max<i64>(0, lo(box.min.x, origin_.x));
    const i64 x1 = std::min<i64>(static_cast<i64>(w_) - 1, hi(box.max.x, origin_.x));
    const i64 z0 = std::max<i64>(0, lo(box.min.z, origin_.z));
    const i64 z1 = std::min<i64>(static_cast<i64>(h_) - 1, hi(box.max.z, origin_.z));
    for (i64 cz = z0; cz <= z1; ++cz)
        for (i64 cx = x0; cx <= x1; ++cx)
            blocked_[static_cast<usize>(cz) * w_ + static_cast<usize>(cx)] = 1u;
}

void FlowField::build(math::Vec3 goal_world) {
    const usize n = static_cast<usize>(w_) * h_;
    cost_.assign(n, kUnreachable);
    flow_.assign(n, math::Vec3{0.0f, 0.0f, 0.0f});
    if (n == 0) return;

    u32 gx = 0, gz = 0;
    if (!to_cell(goal_world, gx, gz)) return;
    const u32 gi = gz * w_ + gx;
    if (blocked_[gi]) return;
    cost_[gi] = 0u;

    // Dijkstra cost field from the goal. Min-heap keyed by (cost, index) so ties
    // are resolved by cell index -> deterministic.
    using QN = std::pair<u32, u32>;  // (cost, cell index)
    std::priority_queue<QN, std::vector<QN>, std::greater<QN>> pq;
    pq.push({0u, gi});
    while (!pq.empty()) {
        const QN top = pq.top();
        pq.pop();
        const u32 c = top.first;
        const u32 idx = top.second;
        if (c > cost_[idx]) continue;  // stale
        const u32 cx = idx % w_;
        const u32 cz = idx / w_;
        for (int k = 0; k < 8; ++k) {
            const int nx = static_cast<int>(cx) + kDX[k];
            const int nz = static_cast<int>(cz) + kDZ[k];
            if (nx < 0 || nz < 0 || nx >= static_cast<int>(w_) ||
                nz >= static_cast<int>(h_))
                continue;
            const u32 ni = static_cast<u32>(nz) * w_ + static_cast<u32>(nx);
            if (blocked_[ni]) continue;
            if (k >= 4) {  // diagonal: don't cut a blocked corner
                const u32 oa = cz * w_ + static_cast<u32>(nx);
                const u32 ob = static_cast<u32>(nz) * w_ + cx;
                if (blocked_[oa] || blocked_[ob]) continue;
            }
            const u32 nc = c + kCost[k];
            if (nc < cost_[ni]) {
                cost_[ni] = nc;
                pq.push({nc, ni});
            }
        }
    }

    // Flow: each free, reachable, non-goal cell points toward its lowest-cost
    // neighbour (ties -> lowest neighbour index for determinism).
    for (u32 cz = 0; cz < h_; ++cz) {
        for (u32 cx = 0; cx < w_; ++cx) {
            const u32 idx = cz * w_ + cx;
            if (blocked_[idx] || cost_[idx] == kUnreachable || cost_[idx] == 0u)
                continue;
            u32 best = cost_[idx];
            int bk = -1;
            for (int k = 0; k < 8; ++k) {
                const int nx = static_cast<int>(cx) + kDX[k];
                const int nz = static_cast<int>(cz) + kDZ[k];
                if (nx < 0 || nz < 0 || nx >= static_cast<int>(w_) ||
                    nz >= static_cast<int>(h_))
                    continue;
                const u32 ni = static_cast<u32>(nz) * w_ + static_cast<u32>(nx);
                if (blocked_[ni] || cost_[ni] == kUnreachable) continue;
                if (cost_[ni] < best) {  // strictly-less => first (lowest k) wins ties
                    best = cost_[ni];
                    bk = k;
                }
            }
            if (bk >= 0) {
                flow_[idx] = math::normalize(
                    math::Vec3{static_cast<f32>(kDX[bk]), 0.0f,
                               static_cast<f32>(kDZ[bk])});
            }
        }
    }
}

math::Vec3 FlowField::sample(math::Vec3 world) const {
    u32 cx = 0, cz = 0;
    if (!to_cell(world, cx, cz)) return {0.0f, 0.0f, 0.0f};
    const u32 idx = cz * w_ + cx;
    if (blocked_[idx]) return {0.0f, 0.0f, 0.0f};
    return flow_[idx];
}

bool FlowField::blocked(u32 cx, u32 cz) const noexcept {
    if (cx >= w_ || cz >= h_) return true;
    return blocked_[cz * w_ + cx] != 0u;
}

u32 FlowField::cost(u32 cx, u32 cz) const noexcept {
    if (cx >= w_ || cz >= h_) return kUnreachable;
    return cost_[cz * w_ + cx];
}

}  // namespace psynder::ai
