// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/camera/FovKick.cpp — dynamic FOV (sprint/ADS) easing. See FovKick.h.

#include "camera/FovKick.h"

#include <algorithm>  // std::clamp
#include <cmath>      // std::isfinite

namespace psynder::camera {

f32 fov_target(const FovParams& p, bool sprinting, bool aiming) noexcept {
    // Priority: aiming (ADS) beats sprinting beats resting.
    if (aiming) return p.ads_deg;
    if (sprinting) return p.sprint_deg;
    return p.base_deg;
}

void fov_init(FovState& s, const FovParams& p) noexcept {
    s.current_deg = p.base_deg;
}

void fov_update(FovState& s, const FovParams& p, bool sprinting, bool aiming,
                f32 dt_s) noexcept {
    const f32 target = fov_target(p, sprinting, aiming);

    // Framerate-independent approach: t = clamp(rate * dt, 0, 1). A non-finite
    // or non-positive dt produces a 0 blend (no move, no NaN). Because t in
    // [0,1], a single step never passes the target (no overshoot).
    f32 t = 0.0f;
    if (std::isfinite(dt_s) && dt_s > 0.0f) {
        t = std::clamp(p.lerp_rate_per_s * dt_s, 0.0f, 1.0f);
    }
    s.current_deg += (target - s.current_deg) * t;
}

}  // namespace psynder::camera
