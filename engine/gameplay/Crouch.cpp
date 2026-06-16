// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/gameplay/Crouch.cpp — see Crouch.h.

#include "gameplay/Crouch.h"

#include <cmath>  // std::isfinite

namespace psynder::gameplay {

namespace {
// Clamp x into [lo, hi]. Branchy (not std::clamp) to keep it simple and
// identical across compilers under strict-FP.
inline f32 clampf(f32 x, f32 lo, f32 hi) noexcept {
    if (x < lo) return lo;
    if (x > hi) return hi;
    return x;
}
}  // namespace

void crouch_init(Crouch& c) noexcept {
    c.height_m = c.stand_height_m;
    c.crouched = 0u;
}

void crouch_update(Crouch& c, bool want_crouch, bool blocked_above,
                   f32 dt_s) noexcept {
    // Guard against a non-finite or non-positive step: leave the state untouched
    // so a paused / malformed tick cannot teleport the height or flip the state.
    if (!std::isfinite(dt_s) || dt_s <= 0.0f) return;

    // You can ask to crouch; you can only stand back up when there is headroom.
    const bool currently_crouched = c.crouched != 0u;
    const bool desired = want_crouch || (currently_crouched && blocked_above);
    c.crouched = desired ? 1u : 0u;

    const f32 target = desired ? c.crouch_height_m : c.stand_height_m;
    const f32 step = c.transition_rate_mps * dt_s;

    // Ease toward the target, clamped so it never overshoots.
    if (c.height_m < target) {
        c.height_m += step;
        if (c.height_m > target) c.height_m = target;
    } else if (c.height_m > target) {
        c.height_m -= step;
        if (c.height_m < target) c.height_m = target;
    }

    // Keep the height inside the legal band regardless of the inputs.
    c.height_m = clampf(c.height_m, c.crouch_height_m, c.stand_height_m);
}

bool is_crouched(const Crouch& c) noexcept {
    return c.crouched != 0u;
}

f32 crouch_fraction(const Crouch& c) noexcept {
    const f32 span = c.stand_height_m - c.crouch_height_m;
    if (span <= 0.0f) return 0.0f;  // equal (or inverted) heights: nothing to ease
    return clampf((c.stand_height_m - c.height_m) / span, 0.0f, 1.0f);
}

f32 crouch_speed_mult(const Crouch& c, f32 stand_speed_mult,
                      f32 crouch_speed_mult_full) noexcept {
    const f32 t = crouch_fraction(c);
    return stand_speed_mult + (crouch_speed_mult_full - stand_speed_mult) * t;
}

}  // namespace psynder::gameplay
