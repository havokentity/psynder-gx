// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/camera/LookSmoothing.cpp — exponential low-pass on raw mouse-look
// deltas. See LookSmoothing.h for the model + determinism contract.

#include "camera/LookSmoothing.h"

#include <algorithm>  // std::clamp
#include <cmath>      // std::isfinite

namespace psynder::camera {

void look_smooth_init(LookSmoothState& s) noexcept {
    s.yaw_vel_deg   = 0.0f;
    s.pitch_vel_deg = 0.0f;
}

void look_smooth(LookSmoothState& s, const LookSmoothParams& p,
                 f32 raw_yaw_delta, f32 raw_pitch_delta,
                 f32& out_yaw, f32& out_pitch) noexcept {
    // smoothing clamps to [0, 0.999] so alpha is floored at 0.001 (> 0): even a
    // pathological smoothing of 1+ still advances every frame (never freezes),
    // and smoothing 0 gives alpha 1 -> exact passthrough.
    const f32 alpha = 1.0f - std::clamp(p.smoothing, 0.0f, 0.999f);

    // Guard non-finite raw deltas (NaN / Inf from a bogus platform read) so they
    // can never poison the carried velocity — treat as 0 for that axis.
    const f32 raw_yaw   = std::isfinite(raw_yaw_delta)   ? raw_yaw_delta   : 0.0f;
    const f32 raw_pitch = std::isfinite(raw_pitch_delta) ? raw_pitch_delta : 0.0f;

    // Exponential low-pass per axis: vel += alpha * (raw - vel). At alpha == 1
    // this collapses to vel := raw (passthrough); smaller alpha lags the raw.
    s.yaw_vel_deg   += alpha * (raw_yaw   - s.yaw_vel_deg);
    s.pitch_vel_deg += alpha * (raw_pitch - s.pitch_vel_deg);

    out_yaw   = s.yaw_vel_deg;
    out_pitch = s.pitch_vel_deg;
}

void look_smooth_reset(LookSmoothState& s) noexcept {
    s.yaw_vel_deg   = 0.0f;
    s.pitch_vel_deg = 0.0f;
}

}  // namespace psynder::camera
