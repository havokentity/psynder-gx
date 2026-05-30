// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/ai/Influence.cpp — see Influence.h.

#include "ai/Influence.h"

#include <algorithm>
#include <cmath>

namespace psynder::ai {

namespace {
// 8-neighbour offsets in a fixed scan order (cardinals first, then diagonals),
// matching FlowField so the lowest-index tie-break is consistent across the lane.
constexpr int kDX[8] = {1, -1, 0, 0, 1, 1, -1, -1};
constexpr int kDZ[8] = {0, 0, 1, -1, 1, -1, 1, -1};
}  // namespace

void InfluenceMap::resize(u32 width, u32 height) {
    w_ = width;
    h_ = height;
    field_.assign(static_cast<usize>(w_) * h_, 0.0f);
}

void InfluenceMap::clear() noexcept {
    std::fill(field_.begin(), field_.end(), 0.0f);
}

void InfluenceMap::add_source(u32 cx, u32 cz, f32 strength, u32 radius) noexcept {
    if (w_ == 0 || h_ == 0) return;
    if (cx >= w_ || cz >= h_) return;  // centre off-grid -> nothing to stamp
    if (radius == 0) return;           // a zero-radius cone contributes nothing

    const f32 inv_radius = 1.0f / static_cast<f32>(radius);

    // Clamp the bounding box of the cone to the grid. radius is a u32 so the
    // window is [cx-radius, cx+radius] x [cz-radius, cz+radius], clamped to
    // [0, w_-1] x [0, h_-1] without unsigned underflow.
    const u32 x0 = (cx > radius) ? (cx - radius) : 0u;
    const u32 z0 = (cz > radius) ? (cz - radius) : 0u;
    const u32 x1 = std::min<u32>(cx + radius, w_ - 1u);
    const u32 z1 = std::min<u32>(cz + radius, h_ - 1u);

    for (u32 z = z0; z <= z1; ++z) {
        const f32 dz = static_cast<f32>(z) - static_cast<f32>(cz);
        for (u32 x = x0; x <= x1; ++x) {
            const f32 dx = static_cast<f32>(x) - static_cast<f32>(cx);
            const f32 dist = std::sqrt(dx * dx + dz * dz);
            // Linear cone: full strength at the centre, 0 at dist == radius, and
            // clamped to 0 beyond. max(0, ...) keeps the corners of the box flat.
            const f32 falloff = 1.0f - dist * inv_radius;
            if (falloff > 0.0f) field_[static_cast<usize>(z) * w_ + x] += strength * falloff;
        }
    }
}

f32 InfluenceMap::value(u32 cx, u32 cz) const noexcept {
    if (cx >= w_ || cz >= h_) return 0.0f;
    return field_[static_cast<usize>(cz) * w_ + cx];
}

math::Vec3 InfluenceMap::down_gradient(u32 cx, u32 cz) const noexcept {
    if (cx >= w_ || cz >= h_) return {0.0f, 0.0f, 0.0f};
    const f32 here = field_[static_cast<usize>(cz) * w_ + cx];
    f32 best = here;
    int bk = -1;
    for (int k = 0; k < 8; ++k) {
        const int nx = static_cast<int>(cx) + kDX[k];
        const int nz = static_cast<int>(cz) + kDZ[k];
        if (nx < 0 || nz < 0 || nx >= static_cast<int>(w_) || nz >= static_cast<int>(h_))
            continue;
        const f32 v = field_[static_cast<usize>(nz) * w_ + static_cast<usize>(nx)];
        if (v < best) {  // strictly-less => first (lowest k) wins ties
            best = v;
            bk = k;
        }
    }
    if (bk < 0) return {0.0f, 0.0f, 0.0f};  // local minimum: nowhere safer to go
    return math::normalize(
        math::Vec3{static_cast<f32>(kDX[bk]), 0.0f, static_cast<f32>(kDZ[bk])});
}

math::Vec3 InfluenceMap::up_gradient(u32 cx, u32 cz) const noexcept {
    if (cx >= w_ || cz >= h_) return {0.0f, 0.0f, 0.0f};
    const f32 here = field_[static_cast<usize>(cz) * w_ + cx];
    f32 best = here;
    int bk = -1;
    for (int k = 0; k < 8; ++k) {
        const int nx = static_cast<int>(cx) + kDX[k];
        const int nz = static_cast<int>(cz) + kDZ[k];
        if (nx < 0 || nz < 0 || nx >= static_cast<int>(w_) || nz >= static_cast<int>(h_))
            continue;
        const f32 v = field_[static_cast<usize>(nz) * w_ + static_cast<usize>(nx)];
        if (v > best) {  // strictly-greater => first (lowest k) wins ties
            best = v;
            bk = k;
        }
    }
    if (bk < 0) return {0.0f, 0.0f, 0.0f};  // local maximum: nowhere richer to go
    return math::normalize(
        math::Vec3{static_cast<f32>(kDX[bk]), 0.0f, static_cast<f32>(kDZ[bk])});
}

}  // namespace psynder::ai
