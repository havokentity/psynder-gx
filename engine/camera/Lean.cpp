// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/camera/Lean.cpp — peek / lean view modifier (lean around cover).
// See Lean.h for the sign convention + determinism contract.

#include "camera/Lean.h"

#include <algorithm>  // std::clamp
#include <cmath>      // std::isfinite

namespace psynder::camera {

f32 lean_input(bool lean_left, bool lean_right) noexcept {
    // Both held (or neither) cancel to centred; otherwise the single held side
    // wins. -1 left, +1 right.
    if (lean_left == lean_right) return 0.0f;  // both or neither => 0
    return lean_right ? 1.0f : -1.0f;
}

void lean_update(LeanState& s, const LeanParams& p, f32 target,
                 f32 dt_s) noexcept {
    // A non-finite target can never enter the ease (would poison amount).
    if (!std::isfinite(target)) return;

    // Clamp the target into the legal lean range so a bogus caller can't drive
    // the eased amount out of [-1, +1].
    const f32 goal = std::clamp(target, -1.0f, 1.0f);

    // Framerate-independent approach: t = clamp(rate * dt, 0, 1). A non-finite
    // or non-positive dt produces a 0 blend (no move, no NaN). Because t in
    // [0, 1], a single step never passes the target (no overshoot); at t == 1
    // it lands exactly on the target.
    f32 t = 0.0f;
    if (std::isfinite(dt_s) && dt_s > 0.0f) {
        t = std::clamp(p.lerp_rate_per_s * dt_s, 0.0f, 1.0f);
    }
    s.amount += (goal - s.amount) * t;
}

LeanOffset lean_sample(const LeanState& s, const LeanParams& p) noexcept {
    // Linear in amount: 0 at centre, +/- the authored peak at full lean.
    // Leaning right (amount > 0) => positive lateral (slide +right) + positive
    // roll (bank into the lean); leaning left mirrors both.
    LeanOffset off;
    off.lateral_m = s.amount * p.max_lateral_m;
    off.roll_deg  = s.amount * p.max_roll_deg;
    return off;
}

void lean_reset(LeanState& s) noexcept {
    s.amount = 0.0f;
}

}  // namespace psynder::camera
