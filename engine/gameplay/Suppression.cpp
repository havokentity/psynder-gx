// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/gameplay/Suppression.cpp — see Suppression.h.

#include "gameplay/Suppression.h"

namespace psynder::gameplay {

namespace {
// Clamp x into [lo, hi]. Branchy (not std::clamp) to keep it constexpr-simple
// and identical across compilers under strict-FP.
inline f32 clampf(f32 x, f32 lo, f32 hi) noexcept {
    if (x < lo) return lo;
    if (x > hi) return hi;
    return x;
}

// Finite test without <cmath> dependency: NaN != itself; +/-inf overflow the
// finite range. Used to reject a bad dt before it touches `level`.
inline bool finite(f32 x) noexcept {
    return x == x && x < 3.4e38f && x > -3.4e38f;
}
}  // namespace

void add_suppression(Suppression& s, f32 amount) noexcept {
    s.level = clampf(s.level + amount, 0.0f, 1.0f);
}

void tick_suppression(Suppression& s, f32 dt_s) noexcept {
    // Guard a non-finite or non-positive dt: a stalled / bad frame must not move
    // (let alone unclamp) the suppression level.
    if (!finite(dt_s) || dt_s <= 0.0f) return;
    s.level = clampf(s.level - s.decay_per_s * dt_s, 0.0f, 1.0f);
}

f32 spread_multiplier(const Suppression& s) noexcept {
    // Lerp 1.0 -> max_spread_mult by level. level is kept in [0,1] by the
    // mutators; clamp defensively so a hand-set value can't escape the range.
    const f32 t = clampf(s.level, 0.0f, 1.0f);
    return 1.0f + (s.max_spread_mult - 1.0f) * t;
}

bool is_suppressed(const Suppression& s, f32 threshold) noexcept {
    return s.level >= threshold;
}

void tick_suppressions(scene::World& w, f32 dt_s) noexcept {
    // Every Suppression decays. Pure per-entity update over the chunk columns: no
    // entity ids, no cross-entity state and no structural changes, so the result
    // is order-independent / deterministic.
    w.for_each_chunk<Suppression>([dt_s](usize n, Suppression* sp) {
        for (usize i = 0; i < n; ++i)
            tick_suppression(sp[i], dt_s);
    });
}

}  // namespace psynder::gameplay
