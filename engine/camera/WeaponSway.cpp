// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/camera/WeaponSway.cpp — weapon-sway viewmodel modifier (the gun lags
// the look, then recenters). See WeaponSway.h for the lag direction + the
// determinism contract.

#include "camera/WeaponSway.h"

#include <algorithm>  // std::clamp
#include <cmath>      // std::isfinite

namespace psynder::camera {

void sway_init(SwayState& s) noexcept {
    s.offset_x = 0.0f;
    s.offset_y = 0.0f;
}

void sway_update(SwayState& s, const SwayParams& p, f32 look_yaw_delta_deg,
                 f32 look_pitch_delta_deg, f32 dt_s) noexcept {
    // Guard non-finite look deltas (NaN / Inf from a bogus platform read) so they
    // can never poison the offset — treat as 0 for that axis (no push).
    const f32 yaw   = std::isfinite(look_yaw_delta_deg)   ? look_yaw_delta_deg   : 0.0f;
    const f32 pitch = std::isfinite(look_pitch_delta_deg) ? look_pitch_delta_deg : 0.0f;

    // The gun LAGS the camera: push OPPOSITE the look delta, scaled and clamped.
    // A still look (delta 0) gives target 0, so the same ease recenters the gun.
    const f32 target_x =
        std::clamp(-yaw * p.sway_scale, -p.max_offset, p.max_offset);
    const f32 target_y =
        std::clamp(-pitch * p.sway_scale, -p.max_offset, p.max_offset);

    // Framerate-independent approach: t = clamp(rate * dt, 0, 1). A non-finite
    // or non-positive dt produces a 0 blend (no move, no NaN). Because t in
    // [0, 1], a single step never passes the target (no overshoot); at t == 1
    // it lands exactly on the target.
    f32 t = 0.0f;
    if (std::isfinite(dt_s) && dt_s > 0.0f) {
        t = std::clamp(p.recenter_rate_per_s * dt_s, 0.0f, 1.0f);
    }

    // Ease each axis toward its target: follows the push AND recenters when still.
    s.offset_x += (target_x - s.offset_x) * t;
    s.offset_y += (target_y - s.offset_y) * t;

    // Final clamp so the offset can never sit outside the authored bound (e.g.
    // if a stale state carried a larger value in before the params changed).
    s.offset_x = std::clamp(s.offset_x, -p.max_offset, p.max_offset);
    s.offset_y = std::clamp(s.offset_y, -p.max_offset, p.max_offset);
}

void sway_reset(SwayState& s) noexcept {
    s.offset_x = 0.0f;
    s.offset_y = 0.0f;
}

}  // namespace psynder::camera
