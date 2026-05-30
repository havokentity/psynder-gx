// SPDX-License-Identifier: MIT OR Apache-2.0
//
// engine/camera/Transition.cpp — smooth camera pose blend (death / kill / spawn
// cam). See Transition.h for the convention + determinism contract.

#include "camera/Transition.h"

#include <algorithm>  // std::clamp
#include <cmath>      // std::isfinite

namespace psynder::camera {

namespace {

// Wrap a degree angle into the canonical [-180, +180] range (Camera.h yaw
// convention) using ONLY add/subtract — NO fmod, NO trig — so the result is
// bit-reproducible under strict-FP. The delta fed in here is bounded (it is a
// difference of two angles already in [-180, 180], so it lives in (-360, 360)
// before wrapping, and a single 360 add/subtract is always enough); the loops
// are belt-and-braces for any caller that pre-wraps differently.
f32 wrap_deg_180(f32 a) noexcept {
    while (a > 180.0f) a -= 360.0f;
    while (a < -180.0f) a += 360.0f;
    return a;
}

f32 lerp(f32 a, f32 b, f32 u) noexcept {
    // Precise-endpoint form: exactly a at u==0 and exactly b at u==1.
    return a + (b - a) * u;
}

}  // namespace

void transition_start(TransitionState& t, const CamPose& from, const CamPose& to,
                      f32 duration_s) noexcept {
    t.from      = from;
    t.to        = to;
    t.duration_s = duration_s;
    t.elapsed_s  = 0.0f;

    // A degenerate (non-finite or non-positive) duration snaps instantly: store
    // the endpoints but never enter the active blend — sample() returns `to`.
    if (std::isfinite(duration_s) && duration_s > 0.0f) {
        t.active = 1u;
    } else {
        t.active = 0u;
    }
}

void transition_tick(TransitionState& t, f32 dt_s) noexcept {
    if (t.active == 0u) return;  // idle / already finished — nothing to advance

    // A non-finite or non-positive dt produces no advance (no move, no NaN), so
    // a stalled / bogus frame clock leaves the transition untouched.
    if (!std::isfinite(dt_s) || dt_s <= 0.0f) return;

    t.elapsed_s += dt_s;

    // Clamp the clock to the duration and latch active off once we land on (or
    // pass) the end of the blend.
    if (t.elapsed_s >= t.duration_s) {
        t.elapsed_s = t.duration_s;
        t.active = 0u;
    }
}

f32 smoothstep01(f32 x) noexcept {
    // Clamp into [0, 1] first so the Hermite curve stays monotone + bounded for
    // out-of-range inputs (clamps to 0 below 0, to 1 above 1).
    const f32 c = std::clamp(x, 0.0f, 1.0f);
    // Classic smoothstep: 3c^2 - 2c^3. Zero slope at both ends; exact 0/1.
    return c * c * (3.0f - 2.0f * c);
}

CamPose transition_sample(const TransitionState& t) noexcept {
    // Normalised, eased progress. A degenerate duration reads as fully done
    // (u == 1 => returns `to`), matching the instant-snap start() behaviour.
    f32 u;
    if (std::isfinite(t.duration_s) && t.duration_s > 0.0f) {
        u = smoothstep01(t.elapsed_s / t.duration_s);
    } else {
        u = 1.0f;
    }

    CamPose out;

    // Position: straight per-component lerp.
    out.position[0] = lerp(t.from.position[0], t.to.position[0], u);
    out.position[1] = lerp(t.from.position[1], t.to.position[1], u);
    out.position[2] = lerp(t.from.position[2], t.to.position[2], u);

    // Yaw: shortest-arc blend. Wrap the delta into [-180, 180] so we always take
    // the short way around the +/-180 seam, scale by u, add, and re-wrap so the
    // sampled yaw stays in the canonical range.
    const f32 yaw_delta = wrap_deg_180(t.to.yaw_deg - t.from.yaw_deg);
    out.yaw_deg = wrap_deg_180(t.from.yaw_deg + yaw_delta * u);

    // Pitch: plain lerp (pitch does not wrap).
    out.pitch_deg = lerp(t.from.pitch_deg, t.to.pitch_deg, u);

    return out;
}

bool transition_active(const TransitionState& t) noexcept {
    return t.active != 0u;
}

}  // namespace psynder::camera
