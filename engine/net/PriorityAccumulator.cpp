// SPDX-License-Identifier: MIT OR Apache-2.0
// Psynder-GX — per-entity send-priority accumulator. Lane 18 (net).
//
// Implementation of the Glenn-Fiedler priority-accumulator declared in
// PriorityAccumulator.h: accumulate() grows each entity by its (clamped) base
// every tick; select() takes the top-K by accumulated priority and resets exactly
// those, so missed entities climb and are never starved. Pure f32 algebra, no
// transcendentals, no RNG — deterministic with lowest-index tie-breaking so the
// same op sequence yields the same selection and residual state across runs and
// platforms (strict-FP net lane, cmake/HotLane.cmake, DESIGN-PSYNDER-GX.md §14).

#include "net/PriorityAccumulator.h"

#include <algorithm>

namespace psynder::net {

void PriorityAccumulator::resize(usize entity_count) {
    // Zero-fill on grow; assign() also clears existing values on a same-size or
    // shrink call, so resize() always returns a freshly-zeroed accumulator set.
    acc_.assign(entity_count, 0.f);
    // Keep the select() scratch capacious enough that the hot path never grows
    // the heap. reserve() is a no-op once it has reached this capacity.
    scratch_.reserve(entity_count);
}

void PriorityAccumulator::reset() noexcept {
    std::fill(acc_.begin(), acc_.end(), 0.f);
}

void PriorityAccumulator::accumulate(std::span<const f32> base_priorities) noexcept {
    // Shorter span uses the min length; a longer span ignores the tail. Negative
    // bases are clamped to 0 so a stale/garbage base cannot drain an accumulator.
    const usize n = std::min(acc_.size(), base_priorities.size());
    for (usize i = 0; i < n; ++i) {
        const f32 base = base_priorities[i];
        if (base > 0.f) acc_[i] += base;  // base <= 0 -> max(0, base) == 0, no-op.
    }
}

void PriorityAccumulator::select(usize max_count, std::vector<u32>& out_indices) noexcept {
    out_indices.clear();
    if (max_count == 0) return;

    // Gather every sendable candidate (acc > 0). Entities with acc <= 0 have
    // nothing to send and are skipped entirely, so they can never be selected.
    scratch_.clear();
    const usize n = acc_.size();
    for (usize i = 0; i < n; ++i) {
        if (acc_[i] > 0.f) scratch_.push_back(static_cast<u32>(i));
    }
    if (scratch_.empty()) return;

    // Rank by DESCENDING accumulated priority; ties break to the LOWEST index.
    // stable_sort is unnecessary — the explicit index tie-break is total and
    // deterministic on its own. acc_ reads here are by index captured by ref.
    std::sort(scratch_.begin(), scratch_.end(),
              [this](u32 a, u32 b) noexcept {
                  const f32 pa = acc_[a];
                  const f32 pb = acc_[b];
                  if (pa != pb) return pa > pb;  // higher priority first.
                  return a < b;                  // tie -> lower index first.
              });

    // Take the top-K (or fewer if there were fewer candidates), emit in ranked
    // order, and reset exactly those accumulators to 0 so they fall to the back
    // of the queue while everyone else keeps climbing.
    const usize take = std::min(max_count, scratch_.size());
    out_indices.reserve(take);
    for (usize k = 0; k < take; ++k) {
        const u32 idx = scratch_[k];
        out_indices.push_back(idx);
        acc_[idx] = 0.f;
    }
}

f32 PriorityAccumulator::priority(usize i) const noexcept {
    return (i < acc_.size()) ? acc_[i] : 0.f;
}

}  // namespace psynder::net
